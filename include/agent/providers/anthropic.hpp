#pragma once

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
    std::string parse_stream(const std::string& chunk, std::string& buffer, bool& done) override;

    JSON build_request(const Conversation& conv, const JSON& tools_schema) override;
    Response parse_response(const JSON& response) override;
    JSON make_tool_result(const std::string& tool_call_id, const std::string& result) override;

private:
    JSON message_to_json(const Message& msg);
    JSON convert_tools(const JSON& tools_schema);
};

} // namespace agent::providers
