#pragma once

#include <optional>
#include <unordered_set>
#include <vector>
#include <utility>
#include "agent/providers/openai.hpp"
#include "agent/auth/kimi_token.hpp"
#include "agent/api/client.hpp"

namespace agent::providers {

class Kimi : public OpenAI {
public:
    Kimi(const Config& cfg);

    std::string name() const override { return "kimi"; }
    std::string endpoint() const override { return build_endpoint("/chat/completions"); }

    // Send the Kimi Code device identity headers (X-Msh-* + User-Agent) on every
    // request so inference is attributed to the CLI the same way the official
    // client does.
    std::vector<std::pair<std::string, std::string>> extra_headers() const override;

    // Add the Moonshot-proprietary `thinking` field to the request body. Kimi's
    // coding model runs with thinking enabled by default.
    JSON build_request(const Conversation& conv, const JSON& tools_schema) override;

    // Called once before each request so the provider can refresh the token or
    // complete interactive login before auth_value() is read.
    void prepare_request(api::Client& client) override;

    // Explicitly authenticate (refresh stored token or run device-code flow).
    // Returns true if a usable token is now available.
    bool authenticate(api::Client& client, bool force_login = false);
    bool ready_noninteractive(api::Client& client) override;

    bool is_authenticated() const { return !_config.api_key.empty(); }

    // Kimi-specific capabilities that the UI or REPL can query.
    std::unordered_set<std::string> capabilities() const override {
        return { "model-command" };
    }

    // Apply provider-specific options from config (e.g. provider.kimi.model).
    void apply_provider_options(const JSON& options) override;

private:
    std::string oauth_host() const;
    std::string oauth_client_id() const;

    std::optional<auth::KimiToken> _token;

    // Thinking is on by default for Kimi's coding model. Configurable via
    // provider.kimi.thinking (off | on | low | medium | high | xhigh | max).
    bool _thinking_enabled = true;
    std::string _thinking_effort; // empty = enabled without an explicit effort
};

} // namespace agent::providers
