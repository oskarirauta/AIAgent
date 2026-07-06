#include "agent/providers/openai.hpp"

#include "throws.hpp"
#include "logger.hpp"

namespace agent::providers {

static long json_long(const JSON& v) {
    if ( v == JSON::TYPE::INT ) return static_cast<long>(static_cast<long long>(v));
    if ( v == JSON::TYPE::FLOAT ) return static_cast<long>(static_cast<long double>(v));
    return 0;
}

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

    if ( msg.role == agent::Role::ASSISTANT && !msg.tool_calls.empty()) {
        JSON calls = JSON::Array{};
        for ( const auto& tc : msg.tool_calls ) {
            JSON call = JSON::Object{
                { "id", tc.id },
                { "type", "function" },
                { "function", JSON::Object{
                    { "name", tc.name },
                    { "arguments", tc.arguments }
                }}
            };
            calls.append(call);
        }
        obj["tool_calls"] = calls;
    }

    return obj;
}

JSON OpenAI::build_request(const Conversation& conv, const JSON& tools_schema) {

    JSON messages = JSON::Array{};
    for ( const auto& msg : request_messages(conv)) {
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

        // Reasoning/thinking content: the de-facto field across OpenAI-compatible
        // reasoners (DeepSeek, Qwen, Moonshot/Kimi) is `reasoning_content`; some
        // gateways use `reasoning`. It is separate from the visible `content`.
        for ( const char* key : { "reasoning_content", "reasoning" } ) {
            if ( msg.contains(key) && msg[key] == JSON::TYPE::STRING ) {
                r.thinking = msg[key].to_string();
                break;
            }
        }

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

    if ( response.contains("usage") && response["usage"] == JSON::TYPE::OBJECT ) {
        JSON u = response["usage"];
        if ( u.contains("prompt_tokens")) r.input_tokens = json_long(u["prompt_tokens"]);
        if ( u.contains("completion_tokens")) r.output_tokens = json_long(u["completion_tokens"]);
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

void OpenAI::stream_reset() {
    _s_content.clear();
    _s_reasoning.clear();
    _s_tools.clear();
    _s_input_tokens = 0;
    _s_output_tokens = 0;
}

StreamChunk OpenAI::parse_stream(const std::string& chunk, std::string& buffer, bool& done) {
    buffer += chunk;
    StreamChunk out;
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
            if ( j.contains("usage") && j["usage"] == JSON::TYPE::OBJECT ) {
                JSON u = j["usage"];
                if ( u.contains("prompt_tokens")) _s_input_tokens = json_long(u["prompt_tokens"]);
                if ( u.contains("completion_tokens")) _s_output_tokens = json_long(u["completion_tokens"]);
            }
            if ( !( j.contains("choices") && j["choices"] == JSON::TYPE::ARRAY && j["choices"].size() > 0 ))
                continue;
            JSON delta = j["choices"][0]["delta"];

            if ( delta.contains("content") && delta["content"] == JSON::TYPE::STRING ) {
                std::string c = delta["content"].to_string();
                _s_content += c;
                out.content += c;
            }
            // reasoning_content (DeepSeek/Qwen/Moonshot-Kimi convention); some use `reasoning`.
            for ( const char* key : { "reasoning_content", "reasoning" } ) {
                if ( delta.contains(key) && delta[key] == JSON::TYPE::STRING ) {
                    std::string rc = delta[key].to_string();
                    _s_reasoning += rc;
                    out.reasoning += rc;
                    break;
                }
            }
            // Tool calls arrive in fragments keyed by `index`; id/name come once,
            // arguments accumulate across deltas.
            if ( delta.contains("tool_calls") && delta["tool_calls"] == JSON::TYPE::ARRAY ) {
                for ( size_t i = 0; i < delta["tool_calls"].size(); ++i ) {
                    JSON tc = delta["tool_calls"][i];
                    int idx = tc.contains("index") ? static_cast<int>(json_long(tc["index"])) : 0;
                    PartialToolCall& p = _s_tools[idx];
                    if ( tc.contains("id") && tc["id"] == JSON::TYPE::STRING )
                        p.id = tc["id"].to_string();
                    if ( tc.contains("function") && tc["function"] == JSON::TYPE::OBJECT ) {
                        JSON fn = tc["function"];
                        if ( fn.contains("name") && fn["name"] == JSON::TYPE::STRING )
                            p.name = fn["name"].to_string();
                        if ( fn.contains("arguments") && fn["arguments"] == JSON::TYPE::STRING )
                            p.arguments += fn["arguments"].to_string();
                    }
                }
            }
        } catch ( const std::exception& e ) {
            // ignore malformed chunks
        }
    }
    return out;
}

Response OpenAI::stream_result() {
    Response r;
    r.message = _s_content;
    r.thinking = _s_reasoning;
    r.input_tokens = _s_input_tokens;
    r.output_tokens = _s_output_tokens;
    for ( const auto& [idx, p] : _s_tools ) {
        if ( p.name.empty())
            continue;
        ToolCall call;
        call.id = p.id;
        call.name = p.name;
        try {
            call.arguments = p.arguments.empty() ? JSON::Object{} : JSON::parse(p.arguments);
        } catch ( const std::exception& ) {
            call.arguments = JSON::Object{};
        }
        r.tool_calls.push_back(call);
    }
    return r;
}

} // namespace agent::providers
