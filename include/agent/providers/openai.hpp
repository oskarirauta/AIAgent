#pragma once

#include <map>
#include "agent/providers/provider.hpp"

namespace agent::providers {

class OpenAI : public Provider {
public:
    OpenAI(const Config& cfg) : Provider(cfg) {}

    std::string name() const override { return "openai"; }
    std::string endpoint() const override { return build_endpoint("/chat/completions"); }
    bool supports_streaming() const override { return true; }

    bool stream_wants_usage() const override { return true; }
    void stream_reset() override;
    StreamChunk parse_stream(const std::string& chunk, std::string& buffer, bool& done) override;
    Response stream_result() override;

    JSON build_request(const Conversation& conv, const JSON& tools_schema) override;
    Response parse_response(const JSON& response) override;
    JSON make_tool_result(const std::string& tool_call_id, const std::string& result) override;

private:
    JSON message_to_json(const Message& msg);

    // Streaming accumulation (reset each turn by stream_reset()).
    struct PartialToolCall { std::string id; std::string name; std::string arguments; };
    std::string _s_content;
    std::string _s_reasoning;
    std::map<int, PartialToolCall> _s_tools; // keyed by the delta's tool_call index
    long _s_input_tokens = 0;
    long _s_output_tokens = 0;
};

} // namespace agent::providers
