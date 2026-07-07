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
    // NOTE: extended-thinking TEXT on the subscription (OAuth) channel is
    // redacted PER MODEL, not per channel (verified live 2026-07-07):
    //   - claude-opus-4-8:   thinking block arrives EMPTY — "thinking":"" plus a
    //     signature_delta; zero thinking_delta events. No client-side setting
    //     unlocks it: tried the full CLI beta set (claude-code-20250219,
    //     interleaved-thinking-2025-05-14, fine-grained-tool-streaming-…), a
    //     claude-cli User-Agent, non-streaming, and tools+interleaved.
    //   - claude-sonnet-4-6: full thinking text streams via thinking_delta and
    //     displays in our UI (💭 …) with /thinking on — no extra betas needed.
    // The model always reasons (and answers better) either way; opus just does
    // not return the plaintext on this channel. The signature must still be
    // preserved across tool calls (Anthropic::message_to_json replays the
    // stored thinking blocks verbatim).
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

    // Apply provider-specific options from config (e.g. provider.claude.model).
    void apply_provider_options(const JSON& options) override;

private:
    std::optional<auth::ClaudeToken> _token;
};

} // namespace agent::providers
