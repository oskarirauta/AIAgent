#pragma once

#include <map>
#include "agent/providers/provider.hpp"

namespace agent::providers {

class OpenAI : public Provider {
public:
    OpenAI(const Config& cfg) : Provider(cfg) {}

    std::string name() const override { return "openai"; }
    std::string endpoint() const override { return build_endpoint("/chat/completions"); }
    std::vector<std::string> list_models(api::Client& client) override;
    bool supports_streaming() const override { return true; }

    void stream_reset() override;
    StreamChunk parse_stream(const std::string& chunk, std::string& buffer, bool& done) override;
    Response stream_result() override;

    JSON build_request(const Conversation& conv, const JSON& tools_schema) override;
    Response parse_response(const JSON& response) override;
    JSON make_tool_result(const std::string& tool_call_id, const std::string& result) override;
    void apply_provider_options(const JSON& options) override;
    void prepare_stream_request(JSON& req) override {
        // Ask for a final usage chunk on streamed responses (OpenAI/OpenRouter/
        // Moonshot); without it a streamed turn reports zero tokens.
        req["stream_options"] = JSON::Object{ { "include_usage", true } };
    }

protected:
    // /thinking maps to the chat-completions `reasoning_effort` (o-series / gpt-5).
    // _reasoning_enabled off = no field. OpenRouter overrides how it is sent.
    bool _reasoning_enabled = false;
    std::string _reasoning_effort; // low | medium | high (empty = provider default)
    virtual void add_reasoning(JSON& req) const; // OpenRouter sends a different shape

private:
    JSON message_to_json(const Message& msg);

    // Streaming accumulation (reset each turn by stream_reset()).
    struct PartialToolCall { std::string id; std::string name; std::string arguments; };
    std::string _s_content;
    std::string _s_reasoning;
    std::map<int, PartialToolCall> _s_tools; // keyed by the delta's tool_call index
    long _s_input_tokens = 0;
    long _s_output_tokens = 0;
    bool _s_truncated = false;
    long _s_cached_tokens = 0;
};

} // namespace agent::providers
