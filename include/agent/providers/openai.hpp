#pragma once

#include "agent/providers/provider.hpp"

namespace agent::providers {

class OpenAI : public Provider {
public:
    OpenAI(const Config& cfg) : Provider(cfg) {}

    std::string name() const override { return "openai"; }
    std::string endpoint() const override { return build_endpoint("/chat/completions"); }
    bool supports_streaming() const override { return true; }
    std::string parse_stream(const std::string& chunk, std::string& buffer, bool& done) override;

    JSON build_request(const Conversation& conv, const JSON& tools_schema) override;
    Response parse_response(const JSON& response) override;
    JSON make_tool_result(const std::string& tool_call_id, const std::string& result) override;

private:
    JSON message_to_json(const Message& msg);
};

} // namespace agent::providers
