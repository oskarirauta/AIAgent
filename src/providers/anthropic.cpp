#include "anthropic.hpp"

#include "throws.hpp"
#include "logger.hpp"

namespace agent::providers {

static std::string role_to_string(agent::Role role) {
    switch ( role ) {
        case agent::Role::SYSTEM: return "system";
        case agent::Role::USER: return "user";
        case agent::Role::ASSISTANT: return "assistant";
        case agent::Role::TOOL: return "user"; // anthropic tool results go as user messages
    }
    return "user";
}

JSON Anthropic::message_to_json(const Message& msg) {
    return JSON::Object{
        { "role", role_to_string(msg.role) },
        { "content", msg.content }
    };
}

JSON Anthropic::convert_tools(const JSON& tools_schema) {
    // tools_schema is OpenAI format: [{"type":"function","function":{"name":..., "description":..., "parameters":...}}]
    // Anthropic wants: [{"name":..., "description":..., "input_schema":...}]
    JSON arr = JSON::Array{};
    if ( tools_schema != JSON::TYPE::ARRAY )
        return arr;

    for ( size_t i = 0; i < tools_schema.size(); i++ ) {
        JSON item = tools_schema[i];
        if ( item.contains("function")) {
            JSON fn = item["function"];
            JSON tool = JSON::Object{
                { "name", fn.contains("name") ? fn["name"].to_string() : "" },
                { "description", fn.contains("description") ? fn["description"].to_string() : "" }
            };
            if ( fn.contains("parameters"))
                tool["input_schema"] = fn["parameters"];
            else
                tool["input_schema"] = JSON::Object{{ "type", "object" }};
            arr.append(tool);
        }
    }
    return arr;
}

JSON Anthropic::build_request(const Conversation& conv, const JSON& tools_schema) {

    std::string system;
    JSON messages = JSON::Array{};

    for ( const auto& msg : conv.messages()) {
        if ( msg.role == agent::Role::SYSTEM ) {
            system = msg.content;
        } else if ( msg.role == agent::Role::TOOL ) {
            // tool result as user message with tool_result block
            JSON content = JSON::Array{
                JSON::Object{
                    { "type", "tool_result" },
                    { "tool_use_id", msg.tool_call_id.value_or("") },
                    { "content", msg.content }
                }
            };
            messages.append(JSON::Object{
                { "role", "user" },
                { "content", content }
            });
        } else {
            messages.append(message_to_json(msg));
        }
    }

    JSON req = JSON::Object{
        { "model", _config.model },
        { "max_tokens", 4096 },
        { "messages", messages }
    };

    if ( !system.empty())
        req["system"] = system;

    JSON tools = convert_tools(tools_schema);
    if ( tools == JSON::TYPE::ARRAY && !tools.empty()) {
        req["tools"] = tools;
    }

    return req;
}

Response Anthropic::parse_response(const JSON& response) {

    Response r;

    if ( !response.contains("content")) {
        r.success = false;
        r.message = "missing content in response";
        return r;
    }

    JSON content = response["content"];
    if ( content == JSON::TYPE::ARRAY ) {
        for ( size_t i = 0; i < content.size(); i++ ) {
            JSON block = content[i];
            std::string type = block.contains("type") ? block["type"].to_string() : "";
            if ( type == "text" && block.contains("text")) {
                r.message += block["text"].to_string();
            } else if ( type == "tool_use" ) {
                ToolCall tc;
                if ( block.contains("id"))
                    tc.id = block["id"].to_string();
                if ( block.contains("name"))
                    tc.name = block["name"].to_string();
                if ( block.contains("input"))
                    tc.arguments = block["input"];
                r.tool_calls.push_back(tc);
            }
        }
    }

    return r;
}

JSON Anthropic::make_tool_result(const std::string& tool_call_id, const std::string& result) {
    return JSON::Object{
        { "role", "user" },
        { "content", JSON::Array{
            JSON::Object{
                { "type", "tool_result" },
                { "tool_use_id", tool_call_id },
                { "content", result }
            }
        }}
    };
}

} // namespace agent::providers
