#include "agent/providers/claude.hpp"

#include <iostream>
#include <thread>
#include <chrono>
#include <random>
#include <fstream>
#include "agent/auth/claude_oauth.hpp"
#include "logger.hpp"
#include "throws.hpp"

namespace agent::providers {

static constexpr const char* CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";

// OAuth state (CSRF token): secure randomness from /dev/urandom, not mt19937.
static std::string random_state(size_t length) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    const int n = static_cast<int>(sizeof(alphabet) - 1);
    const int limit = 256 - (256 % n);

    std::ifstream urandom("/dev/urandom", std::ios::binary);
    std::random_device rd;
    std::string s;
    s.reserve(length);
    while ( s.size() < length ) {
        int b = -1;
        if ( urandom ) {
            unsigned char byte;
            if ( urandom.read(reinterpret_cast<char*>(&byte), 1))
                b = byte;
        }
        if ( b < 0 )
            b = static_cast<int>(rd() & 0xff);
        if ( b < limit )
            s += alphabet[b % n];
    }
    return s;
}

Claude::Claude(const Config& cfg) : Anthropic(cfg) {
    // Default to Anthropic's endpoint, but keep a URL the user set explicitly.
    if ( _config.api_url == Config().api_url )
        _config.api_url = "https://api.anthropic.com";
    _token = auth::load_claude_token(_config.home_dir);
}

bool Claude::authenticate(api::Client& client, bool force_login) {
    // 1. Try refreshing an existing token first (unless forced re-login).
    if ( !force_login && _token && !_token->refresh_token.empty()) {
        try {
            std::string previous_refresh = _token->refresh_token;
            auto refreshed = auth::refresh_access_token(client, _token->refresh_token);
            // Anthropic rotates the refresh token; if the response omits a new one,
            // keep the old (and log it — reusing a rotated-out token 401s next time).
            bool rotated = !refreshed.refresh_token.empty();
            if ( !rotated )
                refreshed.refresh_token = previous_refresh;
            _token = refreshed;
            auth::save_claude_token(_config.home_dir, *_token);
            logger::info["claude"] << "access token refreshed (expires_in=" << _token->expires_in
                                   << "s, new_refresh_token=" << ( rotated ? "yes" : "no" ) << ")" << std::endl;
            return true;
        } catch ( const std::exception& e ) {
            logger::warning["claude"] << "token refresh failed: " << e.what() << std::endl;
            _token = std::nullopt;
        }
    }

    // 2. Run manual OAuth authorization-code flow with PKCE.
    // This avoids requiring a local browser / callback listener.
    auto pkce = auth::generate_pkce();
    std::string state = random_state(32);

    std::string url = auth::authorization_url(CLIENT_ID, agent::auth::MANUAL_REDIRECT_URI, state, pkce.challenge);

    std::cout << "\n";
    std::cout << "Claude login required.\n";
    std::cout << "Open this URL in a browser and log in:\n";
    std::cout << url << "\n\n";
    std::cout.flush();

    std::string code = auth::prompt_for_authorization_code();
    if ( code.empty())
        throws << "claude login failed: no authorization code entered" << std::endl;

    auto token = auth::exchange_code(client, CLIENT_ID, code, pkce.verifier, agent::auth::MANUAL_REDIRECT_URI, state);
    auth::save_claude_token(_config.home_dir, token);
    _token = token;

    std::cout << " done\n\n";
    return true;
}

bool Claude::ready_noninteractive(api::Client& client) {
    // Used by a mid-session /provider switch: refresh silently, never prompt.
    if ( !_token || _token->access_token.empty())
        return false;
    if ( auth::token_needs_refresh(*_token, 300)) {
        if ( _token->refresh_token.empty())
            return false;
        try {
            std::string previous_refresh = _token->refresh_token;
            auto refreshed = auth::refresh_access_token(client, _token->refresh_token);
            if ( refreshed.refresh_token.empty())
                refreshed.refresh_token = previous_refresh;
            _token = refreshed;
            auth::save_claude_token(_config.home_dir, *_token);
        } catch ( const std::exception& e ) {
            logger::warning["claude"] << "token refresh failed on switch: " << e.what() << std::endl;
            return false;
        }
    }
    return true;
}

void Claude::prepare_request(api::Client& client) {
    if ( !_token || _token->access_token.empty() || auth::token_needs_refresh(*_token, 300)) {
        authenticate(client, false);
    }
}

JSON Claude::build_request(const Conversation& conv, const JSON& tools_schema) {
    JSON req = Anthropic::build_request(conv, tools_schema);

    // Claude Code's OAuth access token is only accepted when the first system
    // block is the official CLI identity. Any user/system prompt is appended as
    // a second block.
    JSON system_blocks = JSON::Array{
        JSON::Object{
            { "type", "text" },
            { "text", "You are Claude Code, Anthropic's official CLI for Claude." }
        }
    };

    // The base may have delivered `system` as a plain string or, when prompt
    // caching is on, as an array of text blocks — carry the user's system prompt
    // over in either case (dropping any cache_control; it is re-added below).
    if ( req.contains("system")) {
        if ( req["system"] == JSON::TYPE::STRING ) {
            std::string existing = req["system"].to_string();
            if ( !existing.empty())
                system_blocks.append(JSON::Object{ { "type", "text" }, { "text", existing } });
        } else if ( req["system"] == JSON::TYPE::ARRAY ) {
            JSON arr = req["system"];
            for ( size_t i = 0; i < arr.size(); ++i )
                if ( arr[i].contains("text"))
                    system_blocks.append(JSON::Object{ { "type", "text" }, { "text", arr[i]["text"].to_string() } });
        }
    }

    // Cache the system prefix (its final block) too.
    if ( _config.prompt_cache && system_blocks.size() > 0 )
        system_blocks[system_blocks.size() - 1]["cache_control"] = JSON::Object{ { "type", "ephemeral" } };

    req["system"] = system_blocks;
    return req;
}

void Claude::apply_provider_options(const JSON& options) {
    // Inherit Anthropic's handling (notably `thinking`) — without this the base
    // options were dropped and Claude's extended thinking never applied.
    Anthropic::apply_provider_options(options);
    if ( options == JSON::TYPE::OBJECT ) {
        if ( options.contains("model") && options["model"] == JSON::TYPE::STRING )
            _config.model = options["model"].to_string();
    }
}

} // namespace agent::providers
