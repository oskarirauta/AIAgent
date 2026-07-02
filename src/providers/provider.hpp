#pragma once

#include <string>
#include <vector>
#include <memory>
#include "json.hpp"
#include "conversation.hpp"
#include "config.hpp"

namespace agent::providers {

struct ToolCall {
    std::string id;
    std::string name;
    JSON arguments;
};

struct Response {
    std::string message;
    std::vector<ToolCall> tool_calls;
    bool success = true;
};

class Provider {
public:
    virtual ~Provider() = default;

    virtual std::string name() const = 0;
    virtual std::string endpoint() const = 0;
    virtual std::string auth_header() const { return "Authorization"; }
    virtual std::string auth_value() const { return _config.api_key.empty() ? "" : "Bearer " + _config.api_key; }
    virtual bool supports_streaming() const { return false; }
    virtual std::string parse_stream(const std::string& chunk, std::string& buffer, bool& done) { return ""; }
    virtual JSON build_request(const Conversation& conv, const JSON& tools_schema) = 0;
    virtual Response parse_response(const JSON& response) = 0;
    virtual JSON make_tool_result(const std::string& tool_call_id, const std::string& result) = 0;

    const Config& config() const { return _config; }

protected:
    Provider(const Config& cfg) : _config(cfg) {}
    Config _config;
};

std::unique_ptr<Provider> create(const Config& cfg);

} // namespace agent::providers
