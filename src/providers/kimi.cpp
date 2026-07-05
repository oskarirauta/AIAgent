#include "agent/providers/kimi.hpp"

#include <iostream>
#include <thread>
#include <chrono>
#include <cctype>
#include "agent/auth/kimi_oauth.hpp"
#include "logger.hpp"
#include "throws.hpp"

namespace agent::providers {

static constexpr const char* DEFAULT_OAUTH_HOST = "https://auth.kimi.com";
static constexpr const char* DEFAULT_CLIENT_ID = "17e5f671-d194-4dfb-9706-5516cb48c098";

Kimi::Kimi(const Config& cfg) : OpenAI(cfg) {
    _token = auth::load_token(_config.home_dir);
    if ( _token && !auth::token_needs_refresh(*_token, 60))
        _config.api_key = _token->access_token;
}

std::string Kimi::oauth_host() const {
    return _config.oauth_host.empty() ? DEFAULT_OAUTH_HOST : _config.oauth_host;
}

std::string Kimi::oauth_client_id() const {
    return _config.oauth_client_id.empty() ? DEFAULT_CLIENT_ID : _config.oauth_client_id;
}

bool Kimi::authenticate(api::Client& client, bool force_login) {
    // 1. Try refreshing an existing token first (unless forced re-login).
    if ( !force_login && _token && !_token->refresh_token.empty()) {
        try {
            auto refreshed = auth::refresh_access_token(oauth_host(), oauth_client_id(), _token->refresh_token, _config.home_dir, client);
            auth::save_token(_config.home_dir, refreshed);
            _token = refreshed;
            _config.api_key = refreshed.access_token;
            logger::info["kimi"] << "access token refreshed" << std::endl;
            return true;
        } catch ( const std::exception& e ) {
            logger::warning["kimi"] << "token refresh failed: " << e.what() << std::endl;
            _token = std::nullopt;
            _config.api_key.clear();
        }
    }

    // 2. Run interactive device-code flow.
    auto device_auth = auth::request_device_authorization(oauth_host(), oauth_client_id(), _config.home_dir, client);

    std::cout << "\n";
    std::cout << "Kimi login required.\n";
    if ( !device_auth.verification_uri_complete.empty()) {
        std::cout << "Visit: " << device_auth.verification_uri_complete << "\n";
    } else if ( !device_auth.verification_uri.empty()) {
        std::cout << "Visit: " << device_auth.verification_uri << "\n";
    }
    std::cout << "Code:  " << device_auth.user_code << "\n";
    std::cout << "Waiting for authorization";
    std::cout.flush();

    auto start = std::chrono::steady_clock::now();
    std::int64_t timeout_seconds = device_auth.expires_in > 0 ? device_auth.expires_in : 600;
    std::int64_t interval_seconds = device_auth.interval > 0 ? device_auth.interval : 5;

    while ( true ) {
        std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if ( elapsed >= timeout_seconds ) {
            std::cout << "\n";
            throws << "kimi login timed out" << std::endl;
        }

        auto result = auth::poll_device_token(oauth_host(), oauth_client_id(), device_auth.device_code, _config.home_dir, client);

        if ( result.status == auth::PollStatus::success ) {
            auth::save_token(_config.home_dir, result.token);
            _token = result.token;
            _config.api_key = result.token.access_token;
            std::cout << " done\n\n";
            return true;
        }

        if ( result.status == auth::PollStatus::expired )
            throws << "kimi login expired" << std::endl;
        if ( result.status == auth::PollStatus::denied )
            throws << "kimi login denied: " << result.error_description << std::endl;

        std::cout << ".";
        std::cout.flush();
    }
}

std::vector<std::pair<std::string, std::string>> Kimi::extra_headers() const {
    return auth::create_device_headers(_config.home_dir);
}

JSON Kimi::build_request(const Conversation& conv, const JSON& tools_schema) {
    JSON req = OpenAI::build_request(conv, tools_schema);

    // Moonshot-proprietary `thinking` field. Sent on every request so the coding
    // model reasons by default, matching the official client's default_thinking.
    if ( _thinking_enabled ) {
        JSON thinking = JSON::Object{ { "type", "enabled" } };
        if ( !_thinking_effort.empty())
            thinking["effort"] = _thinking_effort;
        req["thinking"] = thinking;
    } else {
        req["thinking"] = JSON::Object{ { "type", "disabled" } };
    }

    return req;
}

void Kimi::prepare_request(api::Client& client) {
    if ( _config.api_key.empty() || !_token || auth::token_needs_refresh(*_token, 300)) {
        authenticate(client, false);
    }
}

void Kimi::apply_provider_options(const JSON& options) {
    if ( options == JSON::TYPE::OBJECT ) {
        if ( options.contains("model") && options["model"] == JSON::TYPE::STRING )
            _config.model = options["model"].to_string();

        // provider.kimi.thinking: off | on/true/enabled | low | medium | high | xhigh | max
        if ( options.contains("thinking") && options["thinking"] == JSON::TYPE::STRING ) {
            std::string v = options["thinking"].to_string();
            std::string lower;
            for ( char c : v ) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            if ( lower == "off" || lower == "false" || lower == "disabled" || lower == "0" ) {
                _thinking_enabled = false;
                _thinking_effort.clear();
            } else if ( lower == "on" || lower == "true" || lower == "enabled" || lower == "1" || lower.empty()) {
                _thinking_enabled = true;
                _thinking_effort.clear();
            } else {
                // treat any other value as an explicit effort level
                _thinking_enabled = true;
                _thinking_effort = lower;
            }
        }
    }
}

} // namespace agent::providers
