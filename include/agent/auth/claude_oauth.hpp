#pragma once

#include <string>
#include <optional>
#include <cstdint>
#include <utility>
#include "agent/api/client.hpp"

namespace agent::auth {

struct ClaudeToken {
    std::string access_token;   // OAuth access token
    std::string refresh_token;  // OAuth refresh token
    std::string api_key;        // Long-lived Anthropic API key for /v1/messages
    std::int64_t expires_at = 0; // unix seconds
    std::string scope;
    std::string token_type = "Bearer";
    std::int64_t expires_in = 0;
};

std::optional<ClaudeToken> load_claude_token(const std::string& home_dir);
void save_claude_token(const std::string& home_dir, const ClaudeToken& token);
void remove_claude_token(const std::string& home_dir);

bool token_needs_refresh(const ClaudeToken& token, std::int64_t margin_seconds = 300);

// PKCE pair: verifier and S256 challenge.
struct PKCE {
    std::string verifier;
    std::string challenge;
};
PKCE generate_pkce();

// Build the OAuth authorization URL.
std::string authorization_url(
    const std::string& client_id,
    const std::string& redirect_uri,
    const std::string& state,
    const std::string& code_challenge);

// Manual redirect URI used when no local browser/callback listener is available.
extern const char* const MANUAL_REDIRECT_URI;

// Prompt the user to paste the authorization code shown by the OAuth provider
// after logging in through the manual redirect page.
std::string prompt_for_authorization_code();

// Exchange the authorization code for tokens.
ClaudeToken exchange_code(
    api::Client& client,
    const std::string& client_id,
    const std::string& code,
    const std::string& code_verifier,
    const std::string& redirect_uri,
    const std::string& state);

// Refresh the access token.
ClaudeToken refresh_access_token(
    api::Client& client,
    const std::string& refresh_token);

// Create a long-lived Anthropic API key from a Console OAuth access token.
// Returns the raw API key (e.g. sk-ant-api03-...).
std::string create_claude_api_key(
    api::Client& client,
    const std::string& access_token);

} // namespace agent::auth
