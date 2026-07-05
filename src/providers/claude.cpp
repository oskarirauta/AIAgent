#include "agent/providers/claude.hpp"

#include <iostream>
#include <thread>
#include <chrono>
#include <random>
#include "agent/auth/claude_oauth.hpp"
#include "logger.hpp"
#include "throws.hpp"

namespace agent::providers {

static constexpr const char* CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";

static std::string random_state(size_t length) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    static std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution<size_t> dist(0, sizeof(alphabet) - 2);
    std::string s;
    s.reserve(length);
    for ( size_t i = 0; i < length; ++i )
        s += alphabet[dist(gen)];
    return s;
}

Claude::Claude(const Config& cfg) : Anthropic(cfg) {
    _config.api_url = "https://api.anthropic.com";
    _token = auth::load_claude_token(_config.home_dir);
}

bool Claude::authenticate(api::Client& client, bool force_login) {
    // 1. Try refreshing an existing token first (unless forced re-login).
    if ( !force_login && _token && !_token->refresh_token.empty()) {
        try {
            std::string previous_refresh = _token->refresh_token;
            auto refreshed = auth::refresh_access_token(client, _token->refresh_token);
            // Some refresh responses omit a new refresh token; keep the old one.
            if ( refreshed.refresh_token.empty())
                refreshed.refresh_token = previous_refresh;
            _token = refreshed;
            auth::save_claude_token(_config.home_dir, *_token);
            logger::info["claude"] << "access token refreshed" << std::endl;
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

    if ( req.contains("system") && req["system"] == JSON::TYPE::STRING ) {
        std::string existing = req["system"].to_string();
        if ( !existing.empty()) {
            system_blocks.append(JSON::Object{
                { "type", "text" },
                { "text", existing }
            });
        }
    }

    req["system"] = system_blocks;
    return req;
}

void Claude::apply_provider_options(const JSON& options) {
    if ( options == JSON::TYPE::OBJECT ) {
        if ( options.contains("model") && options["model"] == JSON::TYPE::STRING )
            _config.model = options["model"].to_string();
    }
}

} // namespace agent::providers
