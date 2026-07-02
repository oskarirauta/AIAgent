#include "agent/providers/openai.hpp"

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

JSON OpenAI::message_to_json(const Message& msg) {
    JSON obj = JSON::Object{
        { "role", role_to_string(msg.role) },
        { "content", msg.content }
    };

    if ( msg.role == agent::Role::TOOL && msg.tool_call_id.has_value()) {
        obj["tool_call_id"] = msg.tool_call_id.value();
    }

    return obj;
}

JSON OpenAI::build_request(const Conversation& conv, const JSON& tools_schema) {

    JSON messages = JSON::Array{};
    for ( const auto& msg : conv.messages()) {
        messages.append(message_to_json(msg));
    }

    JSON req = JSON::Object{
        { "model", _config.model },
        { "messages", messages }
    };

    if ( tools_schema == JSON::TYPE::ARRAY && !tools_schema.empty()) {
        req["tools"] = tools_schema;
    }

    return req;
}

Response OpenAI::parse_response(const JSON& response) {

    Response r;

    if ( !response.contains("choices")) {
        r.success = false;
        r.message = "missing choices in response";
        return r;
    }

    JSON choices = response["choices"];
    if ( choices == JSON::TYPE::ARRAY && choices.size() > 0 ) {
        JSON msg = choices[0]["message"];
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
                    if ( fn.contains("arguments"))
                        call.arguments = JSON::parse(fn["arguments"].to_string());
                    r.tool_calls.push_back(call);
                }
            }
        }
    }

    return r;
}

JSON OpenAI::make_tool_result(const std::string& tool_call_id, const std::string& result) {
    return JSON::Object{
        { "role", "tool" },
        { "tool_call_id", tool_call_id },
        { "content", result }
    };
}

std::string OpenAI::parse_stream(const std::string& chunk, std::string& buffer, bool& done) {
    buffer += chunk;
    std::string out;
    size_t pos;
    while ((pos = buffer.find("\n\n")) != std::string::npos) {
        std::string frame = buffer.substr(0, pos);
        buffer.erase(0, pos + 2);

        size_t data_pos = frame.find("data: ");
        if ( data_pos == std::string::npos )
            continue;

        std::string data = frame.substr(data_pos + 6);
        if ( data == "[DONE]" ) {
            done = true;
            continue;
        }

        try {
            JSON j = JSON::parse(data);
            if ( j.contains("choices") && j["choices"] == JSON::TYPE::ARRAY && j["choices"].size() > 0 ) {
                JSON delta = j["choices"][0]["delta"];
                if ( delta.contains("content") && delta["content"] != nullptr )
                    out += delta["content"].to_string();
            }
        } catch ( const std::exception& e ) {
            // ignore malformed chunks
        }
    }
    return out;
}

} // namespace agent::providers
