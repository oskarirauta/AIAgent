#pragma once

#include <optional>
#include <string>
#include <vector>
#include <unordered_set>

#include "agent/auth/gemini_oauth.hpp"
#include "agent/providers/provider.hpp"

namespace agent::providers {

class Gemini : public Provider {
public:
    explicit Gemini(const Config& cfg);

    std::string name() const override { return "gemini"; }
    std::string endpoint() const override;
    std::string stream_endpoint() const override;

    std::vector<std::string> list_models(api::Client& client) override;

    std::string auth_header() const override;
    std::string auth_value() const override;
    std::vector<std::pair<std::string, std::string>> extra_headers() const override;

    void prepare_request(api::Client& client) override;
    bool authenticate(api::Client& client, bool force = false) override;
    bool ready_noninteractive(api::Client& client) override;
    bool reauthenticate(api::Client& client) override;

    bool supports_streaming() const override { return true; }
    bool supports_tools() const override { return true; }
    bool supports_reasoning() const override { return true; }
    std::unordered_set<std::string> capabilities() const override { return { "subscription", "thinking" }; }

    JSON build_request(const Conversation& conv, const JSON& tools_schema) override;
    Response parse_response(const JSON& response) override;
    JSON make_tool_result(const std::string& tool_call_id, const std::string& result) override;
    void apply_provider_options(const JSON& options) override;
    void prepare_stream_request(JSON& req) override;

    void stream_reset() override;
    StreamChunk parse_stream(const std::string& chunk, std::string& buffer, bool& done) override;
    Response stream_result() override;

    // Helpers
    static long output_cap_for(const std::string& model);
    static long thinking_budget_for(const std::string& effort, const std::string& model);

private:
    bool refresh_now(api::Client& client);
    JSON message_to_parts(const Message& msg) const;

    std::optional<auth::GeminiToken> _token;
    std::string _project_id;
    bool _thinking_enabled = true;
    long _thinking_budget = 2048;
    std::string _thinking_effort = "medium";

    // Streaming accumulation state
    std::string _s_content;
    std::string _s_reasoning;
    std::vector<ToolCall> _s_tools;
    long _s_input_tokens = 0;
    long _s_output_tokens = 0;
    long _s_cached_tokens = 0;
    bool _s_truncated = false;
    bool _s_success = true;
    std::string _s_error;
};

} // namespace agent::providers
