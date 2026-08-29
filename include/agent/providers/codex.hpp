#pragma once

#include <map>
#include <optional>

#include "agent/auth/codex_oauth.hpp"
#include "agent/providers/provider.hpp"

namespace agent::providers {

class Codex : public Provider {
public:
    explicit Codex(const Config& cfg);

    std::string name() const override { return "codex"; }
    std::string endpoint() const override { return build_endpoint("/responses"); }
    bool supports_streaming() const override { return true; }
    bool supports_reasoning() const override { return true; }
    std::unordered_set<std::string> capabilities() const override { return { "subscription" }; }

    std::string auth_value() const override;
    std::vector<std::pair<std::string, std::string>> extra_headers() const override;
    bool authenticate(api::Client& client, bool force = false) override;
    bool ready_noninteractive(api::Client& client) override;
    bool reauthenticate(api::Client& client) override;
    void prepare_request(api::Client& client) override;

    JSON build_request(const Conversation& conv, const JSON& tools_schema) override;
    Response parse_response(const JSON& response) override;
    JSON make_tool_result(const std::string& tool_call_id, const std::string& result) override;
    void apply_provider_options(const JSON& options) override;

    void stream_reset() override;
    StreamChunk parse_stream(const std::string& chunk, std::string& buffer, bool& done) override;
    Response stream_result() override;

private:
    bool refresh_now(api::Client& client);
    void capture_output_item(const JSON& item);

    std::optional<auth::CodexToken> _token;
    // Codex is much easier to read with reasoning off by default; users can
    // still enable it explicitly via /thinking if they want it.
    bool _reasoning_enabled = false;
    std::string _reasoning_effort = "medium";
    std::string _s_content, _s_reasoning;
    std::map<std::string, ToolCall> _s_tools;
    long _s_input_tokens = 0, _s_output_tokens = 0, _s_cached_tokens = 0;
    bool _s_truncated = false;
};

} // namespace agent::providers
