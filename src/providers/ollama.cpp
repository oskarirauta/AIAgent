#include "ollama.hpp"

#include "throws.hpp"
#include "logger.hpp"

namespace agent::providers {

static std::string role_to_string(agent::Role role) {
    switch ( role ) {
        case agent::Role::SYSTEM: return "system";
        case agent::Role::USER: return "user";
        case agent::Role::ASSISTANT: return "assistant";
        case agent::Role::TOOL: return "tool";
    }
    return "user";
}

JSON Ollama::message_to_json(const Message& msg) {
    JSON obj = JSON::Object{
        { "role", role_to_string(msg.role) },
        { "content", msg.content }
    };
    return obj;
}

JSON Ollama::build_request(const Conversation& conv, const JSON& tools_schema) {

    JSON messages = JSON::Array{};
    for ( const auto& msg : conv.messages()) {
        messages.append(message_to_json(msg));
    }

    JSON req = JSON::Object{
        { "model", _config.model },
        { "messages", messages },
        { "stream", false }
    };

    if ( tools_schema == JSON::TYPE::ARRAY && !tools_schema.empty()) {
        req["tools"] = tools_schema;
    }

    return req;
}

Response Ollama::parse_response(const JSON& response) {

    Response r;

    if ( !response.contains("message")) {
        r.success = false;
        r.message = "missing message in response";
        return r;
    }

    JSON msg = response["message"];
    if ( msg.contains("content") && msg["content"] != nullptr )
        r.message = msg["content"].to_string();

    if ( msg.contains("tool_calls") && msg["tool_calls"] == JSON::TYPE::ARRAY ) {
        for ( size_t i = 0; i < msg["tool_calls"].size(); i++ ) {
            JSON tc = msg["tool_calls"][i];
            if ( tc.contains("function")) {
                JSON fn = tc["function"];
                ToolCall call;
                if ( tc.contains("id"))
                    call.id = tc["id"].to_string();
                if ( fn.contains("name"))
                    call.name = fn["name"].to_string();
                if ( fn.contains("arguments")) {
                    JSON args = fn["arguments"];
                    if ( args == JSON::TYPE::STRING )
                        call.arguments = JSON::parse(args.to_string());
                    else
                        call.arguments = args;
                }
                r.tool_calls.push_back(call);
            }
        }
    }

    return r;
}

JSON Ollama::make_tool_result(const std::string& tool_call_id, const std::string& result) {
    return JSON::Object{
        { "role", "tool" },
        { "content", result }
    };
}

} // namespace agent::providers
