#pragma once

#include <map>
#include "agent/providers/provider.hpp"

namespace agent::providers {

class Anthropic : public Provider {
public:
    Anthropic(const Config& cfg) : Provider(cfg) {}

    std::string name() const override { return "anthropic"; }
    std::string endpoint() const override { return build_endpoint("/v1/messages"); }
    std::string auth_header() const override { return "x-api-key"; }
    std::string auth_value() const override { return _config.api_key; }
    bool supports_streaming() const override { return true; }
    std::vector<std::pair<std::string, std::string>> extra_headers() const override {
        return { { "anthropic-version", "2023-06-01" } };
    }
    void stream_reset() override;
    StreamChunk parse_stream(const std::string& chunk, std::string& buffer, bool& done) override;
    Response stream_result() override;

    JSON build_request(const Conversation& conv, const JSON& tools_schema) override;
    Response parse_response(const JSON& response) override;
    JSON make_tool_result(const std::string& tool_call_id, const std::string& result) override;
    void apply_provider_options(const JSON& options) override;

    // Extended-thinking budget (tokens) for an effort level and model. `max` is
    // the model's ceiling (opus ~24k, sonnet ~56k) — a Claude-specific top level.
    static long thinking_budget_for(const std::string& effort, const std::string& model);

protected:
    bool _thinking_enabled = false;   // off by default (unlike Kimi)
    std::string _thinking_effort;     // low|medium|high|xhigh|max ("" = enabled, default budget)

    // Streaming accumulation (reset each turn by stream_reset()).
    struct StreamBlock {
        std::string type; std::string id; std::string name; std::string json;
        std::string thinking;   // thinking text (thinking_delta)
        std::string signature;  // thinking signature (signature_delta) — required for replay
        std::string redacted;   // redacted_thinking data (complete at content_block_start)
    };
    std::string _s_content;
    std::string _s_reasoning;
    std::map<int, StreamBlock> _s_blocks; // by content-block index
    long _s_input_tokens = 0;
    long _s_output_tokens = 0;

    // Add cache_control breakpoints to the stable prefix (tools/system/last
    // message) of a built request. No-op fields are left untouched.
    void apply_cache_control(JSON& req);

private:
    JSON message_to_json(const Message& msg);
    JSON convert_tools(const JSON& tools_schema);
};

} // namespace agent::providers
