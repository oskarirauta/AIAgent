#pragma once

#include <optional>
#include "agent/providers/anthropic.hpp"
#include "agent/auth/claude_oauth.hpp"
#include "agent/api/client.hpp"

namespace agent::providers {

class Claude : public Anthropic {
public:
    Claude(const Config& cfg);

    std::string name() const override { return "claude"; }

    // Claude can consult a stronger sibling model (the /advisor tool) and drive
    // background sub-agent workflows (the /workflows tool), both reached with the
    // same OAuth token.
    std::unordered_set<std::string> capabilities() const override { return { "advisor", "workflows" }; }

    // Subscription (OAuth) requests authenticate with the raw access token as a
    // Bearer credential — NOT an API key. Using an API key here would bill the
    // pay-as-you-go API instead of the Claude subscription.
    std::string auth_header() const override { return "Authorization"; }
    std::string auth_value() const override {
        return ( _token && !_token->access_token.empty()) ? "Bearer " + _token->access_token : "";
    }

    // Anthropic's OAuth beta header is required for the access token to be
    // accepted against /v1/messages, alongside the usual API version header.
    //
    // NOTE: the subscription (OAuth) API redacts extended-thinking TEXT — a
    // thinking block streams only a `signature_delta` (an opaque signed blob),
    // never `thinking_delta`, so `Response::thinking` is always empty for Claude.
    // The model still reasons (and answers better); the plaintext just is not
    // returned over this channel. Verified against forced computation with the
    // claude-code / interleaved-thinking betas added — none unlock the text. So
    // do not expect a live thinking transcript for Claude the way Kimi provides
    // one. The signature must still be preserved across tool calls (Anthropic::
    // message_to_json replays the stored thinking blocks verbatim).
    //
    // Re-verified 2026-07-07 with the full CLI beta set (claude-code-20250219,
    // interleaved-thinking-2025-05-14, fine-grained-tool-streaming-2025-05-14)
    // and a claude-cli User-Agent: the thinking block arrives, but with only a
    // signature_delta — 0 thinking_delta events. Server-side redaction stands.
    std::vector<std::pair<std::string, std::string>> extra_headers() const override {
        return {
            { "anthropic-version", "2023-06-01" },
            { "anthropic-beta", "oauth-2025-04-20" }
        };
    }

    // Prepend the Claude Code identity as the first system block; the OAuth API
    // rejects the token otherwise.
    JSON build_request(const Conversation& conv, const JSON& tools_schema) override;

    // Called once before each request so the provider can refresh the token or
    // complete interactive login before auth_value() is read.
    void prepare_request(api::Client& client) override;

    // Explicitly authenticate (refresh stored token or run OAuth browser flow).
    // Returns true if a usable token is now available.
    bool authenticate(api::Client& client, bool force_login = false);
    bool ready_noninteractive(api::Client& client) override;

    // Claude authenticates with an OAuth token (_token), never _config.api_key.
    bool is_authenticated() const { return _token.has_value(); }

    // Apply provider-specific options from config (e.g. provider.claude.model).
    void apply_provider_options(const JSON& options) override;

private:
    std::optional<auth::ClaudeToken> _token;
};

} // namespace agent::providers
