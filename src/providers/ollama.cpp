#include "agent/providers/ollama.hpp"

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
    for ( const auto& msg : request_messages(conv)) {
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

void Ollama::stream_reset() {
    _s_content.clear();
    _s_reasoning.clear();
    _s_tools.clear();
}

StreamChunk Ollama::parse_stream(const std::string& chunk, std::string& buffer, bool& done) {
    // Ollama's native /api/chat streams newline-delimited JSON — one complete object
    // per '\n', with no SSE `data:` framing or blank-line separators. Process each
    // complete line; keep any trailing partial line buffered for the next chunk.
    for ( char ch : chunk )
        if ( ch != '\r' ) buffer += ch;
    StreamChunk out;
    size_t pos;
    while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string data = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        size_t nsp = data.find_first_not_of(" \t");
        if ( nsp == std::string::npos )
            continue; // blank line
        data = data.substr(nsp);
        try {
            JSON j = JSON::parse(data);
            if ( j.contains("message")) {
                JSON msg = j["message"];
                if ( msg.contains("content") && msg["content"] == JSON::TYPE::STRING ) {
                    std::string c = msg["content"].to_string();
                    _s_content += c;
                    out.content += c;
                }
                if ( msg.contains("thinking") && msg["thinking"] == JSON::TYPE::STRING ) {
                    std::string t = msg["thinking"].to_string();
                    _s_reasoning += t;
                    out.reasoning += t;
                }
                // Ollama emits complete tool_calls (not fragmented across chunks).
                if ( msg.contains("tool_calls") && msg["tool_calls"] == JSON::TYPE::ARRAY ) {
                    for ( size_t i = 0; i < msg["tool_calls"].size(); ++i ) {
                        JSON tc = msg["tool_calls"][i];
                        if ( !tc.contains("function")) continue;
                        JSON fn = tc["function"];
                        ToolCall call;
                        call.id = tc.contains("id") ? tc["id"].to_string()
                                                    : ( "call_" + std::to_string(_s_tools.size()));
                        if ( fn.contains("name")) call.name = fn["name"].to_string();
                        if ( fn.contains("arguments")) {
                            JSON args = fn["arguments"];
                            call.arguments = ( args == JSON::TYPE::STRING ) ? JSON::parse(args.to_string()) : args;
                        }
                        _s_tools.push_back(call);
                    }
                }
            }
            if ( j.contains("done") && j["done"].to_bool())
                done = true;
        } catch ( const std::exception& e ) {
            // ignore malformed chunks
        }
    }
    return out;
}

Response Ollama::stream_result() {
    Response r;
    r.message = _s_content;
    r.thinking = _s_reasoning;
    r.tool_calls = _s_tools;
    return r;
}

std::vector<std::string> Ollama::list_models(api::Client& client) {
    std::vector<std::string> out;
    try {
        // Ollama lists the models installed on the local server at /api/tags.
        JSON j = JSON::parse(client.get(build_endpoint("/api/tags"), {}, nullptr, false, 0, 8));
        if ( j.contains("models") && j["models"] == JSON::TYPE::ARRAY )
            for ( size_t i = 0; i < j["models"].size(); ++i ) {
                JSON m = j["models"][i];
                if ( m.contains("name") && m["name"] == JSON::TYPE::STRING )
                    out.push_back(m["name"].to_string());
            }
    } catch ( ... ) {}
    return out;
}

} // namespace agent::providers
