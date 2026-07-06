#pragma once

#include "agent/providers/provider.hpp"

namespace agent::providers {

class Ollama : public Provider {
public:
    Ollama(const Config& cfg) : Provider(cfg) {}

    std::string name() const override { return "ollama"; }
    std::string endpoint() const override { return build_endpoint("/api/chat"); }
    bool supports_streaming() const override { return true; }

    void stream_reset() override;
    StreamChunk parse_stream(const std::string& chunk, std::string& buffer, bool& done) override;
    Response stream_result() override;

    JSON build_request(const Conversation& conv, const JSON& tools_schema) override;
    Response parse_response(const JSON& response) override;
    JSON make_tool_result(const std::string& tool_call_id, const std::string& result) override;

private:
    JSON message_to_json(const Message& msg);

    std::string _s_content;
    std::string _s_reasoning;
    std::vector<ToolCall> _s_tools;
};

} // namespace agent::providers
