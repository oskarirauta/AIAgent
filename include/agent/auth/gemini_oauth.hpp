#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "agent/api/client.hpp"

namespace agent::auth {

struct GeminiToken {
    std::string access_token;
    std::string refresh_token;
    std::string id_token;
    std::string token_type = "Bearer";
    std::string scope;
    std::string email;
    std::int64_t expiry_date_ms = 0; // Epoch milliseconds, matching Google's oauth_creds.json
    std::string api_key;
};

// Returns path to ~/.gemini/oauth_creds.json (shared with Google Gemini CLI)
std::string gemini_cli_auth_path();

// Returns path to <home_dir>/credentials/gemini.json (AIAgent internal)
std::string gemini_agent_auth_path(const std::string& home_dir = "");

// Attempts to load Gemini OAuth token from either ~/.gemini/oauth_creds.json
// or AIAgent's credentials directory.
std::optional<GeminiToken> load_gemini_token(const std::string& home_dir = "");

// Saves an API key directly into <home_dir>/credentials/gemini.json, preserving any existing OAuth token.
void save_gemini_api_key(const std::string& home_dir, const std::string& api_key);

// Loads a saved API key from <home_dir>/credentials/gemini.json if present.
std::string load_gemini_api_key(const std::string& home_dir = "");

// Checks if the token is expired or will expire within margin_seconds (default 60s).
bool gemini_token_needs_refresh(const GeminiToken& token, std::int64_t margin_seconds = 60);

// Refreshes the token using Google's OAuth2 token endpoint.
GeminiToken refresh_gemini_token(api::Client& client, const GeminiToken& token);

// Saves token to AIAgent's credentials directory or ~/.gemini/oauth_creds.json
void save_gemini_token(const std::string& home_dir, const GeminiToken& token);

// Interactive OAuth login flow for Gemini (browser authorization + code paste).
GeminiToken login_gemini(api::Client& client, const std::string& home_dir = "");

} // namespace agent::auth
