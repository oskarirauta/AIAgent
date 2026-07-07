#include "agent/providers/openai.hpp"

#include "throws.hpp"
#include "logger.hpp"
#include <cctype>

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

    add_reasoning(req);
    return req;
}

void OpenAI::apply_provider_options(const JSON& options) {
    if ( options != JSON::TYPE::OBJECT )
        return;
    if ( options.contains("model") && options["model"] == JSON::TYPE::STRING )
        _config.model = options["model"].to_string();
    if ( options.contains("thinking") && options["thinking"] == JSON::TYPE::STRING ) {
        std::string v;
        for ( char c : options["thinking"].to_string()) v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if ( v == "off" || v == "false" || v == "disabled" || v == "0" ) {
            _reasoning_enabled = false; _reasoning_effort.clear();
        } else if ( v == "on" || v == "true" || v == "enabled" || v == "1" || v.empty()) {
            _reasoning_enabled = true; _reasoning_effort = "medium";
        } else {
            if ( v == "xhigh" || v == "max" ) v = "high"; // chat-completions caps at high
            _reasoning_enabled = true; _reasoning_effort = v;
        }
    }
}

void OpenAI::add_reasoning(JSON& req) const {
    // Chat-completions `reasoning_effort`. Only sent when the user enabled
    // thinking, so ordinary models (which reject the field) are unaffected.
    if ( _reasoning_enabled && !_reasoning_effort.empty())
        req["reasoning_effort"] = _reasoning_effort;
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
        if ( choices[0].contains("finish_reason") &&
             choices[0]["finish_reason"].to_string() == "length" )
            r.truncated = true;
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
                    // Guard the arguments parse like the streaming path does: a
                    // malformed/empty arguments string must not throw out of here.
                    if ( fn.contains("arguments")) {
                        std::string raw = fn["arguments"].to_string();
                        if ( !raw.empty()) {
                            try { call.arguments = JSON::parse(raw); }
                            catch ( ... ) { call.arguments = JSON::Object{}; }
                        }
                    }
                    r.tool_calls.push_back(call);
                }
            }
        }
    }

    if ( response.contains("usage") && response["usage"] == JSON::TYPE::OBJECT ) {
        JSON u = response["usage"];
        if ( u.contains("prompt_tokens")) r.input_tokens = json_long(u["prompt_tokens"]);
        if ( u.contains("completion_tokens")) r.output_tokens = json_long(u["completion_tokens"]);
        if ( u.contains("prompt_tokens_details") && u["prompt_tokens_details"] == JSON::TYPE::OBJECT &&
             u["prompt_tokens_details"].contains("cached_tokens"))
            r.cached_input_tokens = json_long(u["prompt_tokens_details"]["cached_tokens"]);
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
    _s_truncated = false;
    _s_cached_tokens = 0;
}

StreamChunk OpenAI::parse_stream(const std::string& chunk, std::string& buffer, bool& done) {
    // Strip CR so CRLF line endings still split on "\n\n" (some gateways send \r\n).
    for ( char ch : chunk )
        if ( ch != '\r' ) buffer += ch;
    StreamChunk out;
    size_t pos;
    while ((pos = buffer.find("\n\n")) != std::string::npos) {
        std::string frame = buffer.substr(0, pos);
        buffer.erase(0, pos + 2);

        // SSE data line — the space after "data:" is optional (Kimi omits it on
        // chunks but includes it on [DONE]).
        size_t data_pos = frame.find("data:");
        if ( data_pos == std::string::npos )
            continue;
        std::string data = frame.substr(data_pos + 5);
        size_t nsp = data.find_first_not_of(" \t");
        data = ( nsp == std::string::npos ) ? "" : data.substr(nsp);
        if ( data == "[DONE]" ) {
            done = true;
            continue;
        }

        try {
            JSON j = JSON::parse(data);
            auto capture_usage = [this](const JSON& u) {
                if ( u != JSON::TYPE::OBJECT ) return;
                if ( u.contains("prompt_tokens")) _s_input_tokens = json_long(u["prompt_tokens"]);
                if ( u.contains("completion_tokens")) _s_output_tokens = json_long(u["completion_tokens"]);
                if ( u.contains("prompt_tokens_details") && u["prompt_tokens_details"] == JSON::TYPE::OBJECT &&
                     u["prompt_tokens_details"].contains("cached_tokens"))
                    _s_cached_tokens = json_long(u["prompt_tokens_details"]["cached_tokens"]);
            };
            // Usage is top-level with stream_options.include_usage, but Kimi puts
            // it inside choices[0] on the final chunk.
            if ( j.contains("usage")) capture_usage(j["usage"]);
            if ( !( j.contains("choices") && j["choices"] == JSON::TYPE::ARRAY && j["choices"].size() > 0 ))
                continue;
            if ( j["choices"][0].contains("usage")) capture_usage(j["choices"][0]["usage"]);
            if ( j["choices"][0].contains("finish_reason") &&
                 j["choices"][0]["finish_reason"].to_string() == "length" )
                _s_truncated = true;
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
    r.truncated = _s_truncated;
    r.cached_input_tokens = _s_cached_tokens;
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

std::vector<std::string> OpenAI::list_models(api::Client& client) {
    std::vector<std::string> out;
    try {
        std::vector<std::pair<std::string, std::string>> h;
        std::string av = auth_value();
        if ( !av.empty()) h.push_back({ auth_header(), av });
        for ( auto& e : extra_headers()) h.push_back(e);
        JSON j = JSON::parse(client.get(build_endpoint("/models"), h, nullptr, false, 0, 8));
        if ( j.contains("data") && j["data"] == JSON::TYPE::ARRAY )
            for ( size_t i = 0; i < j["data"].size(); ++i ) {
                JSON m = j["data"][i];
                if ( m.contains("id") && m["id"] == JSON::TYPE::STRING )
                    out.push_back(m["id"].to_string());
            }
    } catch ( ... ) {}
    return out;
}

} // namespace agent::providers
