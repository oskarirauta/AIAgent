#include "agent/providers/codex.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>

#include "logger.hpp"
#include "throws.hpp"

namespace agent::providers {

namespace {
long json_long(const JSON& v) {
    if ( v == JSON::TYPE::INT ) return static_cast<long>(static_cast<long long>(v));
    if ( v == JSON::TYPE::FLOAT ) return static_cast<long>(static_cast<long double>(v));
    return 0;
}

JSON responses_tool(const JSON& tool) {
    if ( tool != JSON::TYPE::OBJECT || !tool.contains("function")) return tool;
    JSON fn = tool["function"];
    JSON out = JSON::Object{{ "type", "function" }};
    if ( fn.contains("name")) out["name"] = fn["name"];
    if ( fn.contains("description")) out["description"] = fn["description"];
    if ( fn.contains("parameters")) out["parameters"] = fn["parameters"];
    out["strict"] = false;
    return out;
}
} // namespace

Codex::Codex(const Config& cfg) : Provider(cfg) {
    if ( _config.api_url == Config().api_url )
        _config.api_url = "https://chatgpt.com/backend-api/codex";
    _token = auth::load_codex_token();
}

std::string Codex::auth_value() const {
    return _token && !_token->access_token.empty() ? "Bearer " + _token->access_token : "";
}

std::vector<std::pair<std::string, std::string>> Codex::extra_headers() const {
    std::vector<std::pair<std::string, std::string>> h = {
        { "OpenAI-Beta", "responses=experimental" },
        { "originator", "codex_cli_rs" },
        { "User-Agent", "ai-agent (Codex-compatible)" }
    };
    if ( _token && !_token->account_id.empty())
        h.push_back({ "ChatGPT-Account-Id", _token->account_id });
    return h;
}

bool Codex::refresh_now(api::Client& client) {
    if ( !_token ) _token = auth::load_codex_token();
    if ( !_token || _token->refresh_token.empty()) return false;
    try {
        *_token = auth::refresh_codex_token(client, *_token);
        auth::save_codex_token(*_token);
        return true;
    } catch ( const std::exception& e ) {
        logger::warning["codex"] << "token refresh failed: " << e.what() << std::endl;
        return false;
    }
}

bool Codex::authenticate(api::Client& client, bool force) {
    _token = auth::load_codex_token();
    if ( force || !_token ) {
        try { _token = auth::login_codex_device(client); }
        catch ( const std::exception& e ) { logger::error["codex"] << e.what() << std::endl; return false; }
    }
    if ( auth::codex_token_needs_refresh(*_token) && !refresh_now(client))
        return false;
    return true;
}

bool Codex::ready_noninteractive(api::Client& client) {
    _token = auth::load_codex_token();
    if ( !_token ) return false;
    return !auth::codex_token_needs_refresh(*_token) || refresh_now(client);
}

bool Codex::reauthenticate(api::Client& client) {
    _token = auth::load_codex_token();
    return refresh_now(client);
}

void Codex::prepare_request(api::Client& client) {
    if ( !_token ) _token = auth::load_codex_token();
    if ( !_token )
        throws << "Codex login required — run `codex login` first" << std::endl;
    // ChatGPT-backed Codex model availability is account-dependent.  Do not
    // silently replace a user-selected model from the curated picker: doing so
    // turns the server's useful 400 into a misleading request for another model.
    if ( auth::codex_token_needs_refresh(*_token) && !refresh_now(client))
        throws << "Codex session could not be refreshed — run `codex login` again" << std::endl;
}

JSON Codex::build_request(const Conversation& conv, const JSON& tools_schema) {
    JSON input = JSON::Array{};
    std::string instructions;
    for ( const auto& msg : request_messages(conv)) {
        if ( msg.role == Role::SYSTEM ) {
            if ( !instructions.empty()) instructions += "\n\n";
            instructions += msg.content;
            continue;
        }
        if ( msg.role == Role::TOOL ) {
            input.append(JSON::Object{
                { "type", "function_call_output" },
                { "call_id", msg.tool_call_id.value_or("") },
                { "output", msg.content }
            });
            continue;
        }

        const bool assistant = msg.role == Role::ASSISTANT;
        if ( !msg.content.empty()) {
            input.append(JSON::Object{
                { "role", assistant ? "assistant" : "user" },
                { "content", JSON::Array{JSON::Object{
                    { "type", assistant ? "output_text" : "input_text" },
                    { "text", msg.content }
                }}}
            });
        }
        if ( assistant ) {
            for ( const auto& tc : msg.tool_calls )
                input.append(JSON::Object{
                    { "type", "function_call" }, { "call_id", tc.id },
                    { "name", tc.name }, { "arguments", tc.arguments }
                });
        }
    }

    JSON req = JSON::Object{
        { "model", request_model() }, { "instructions", instructions }, { "input", input },
        { "store", false }, { "parallel_tool_calls", true },
        { "tool_choice", "auto" },
        { "include", JSON::Array{ "reasoning.encrypted_content" } }
    };
    if ( tools_schema == JSON::TYPE::ARRAY && !tools_schema.empty()) {
        JSON tools = JSON::Array{};
        for ( size_t i = 0; i < tools_schema.size(); ++i ) tools.append(responses_tool(tools_schema[i]));
        req["tools"] = tools;
    }
    if ( _reasoning_enabled )
        req["reasoning"] = JSON::Object{{ "effort", _reasoning_effort }, { "summary", "auto" }};
    return req;
}

void Codex::capture_output_item(const JSON& item) {
    if ( item != JSON::TYPE::OBJECT || !item.contains("type")) return;
    const std::string type = item["type"].to_string();
    if ( type == "message" && item.contains("content") && item["content"] == JSON::TYPE::ARRAY ) {
        for ( size_t i = 0; i < item["content"].size(); ++i ) {
            JSON part = item["content"][i];
            if ( part.contains("type") && part["type"].to_string() == "output_text" && part.contains("text"))
                _s_content += part["text"].to_string();
        }
    } else if ( type == "function_call" ) {
        ToolCall tc;
        if ( item.contains("call_id")) tc.id = item["call_id"].to_string();
        if ( item.contains("name")) tc.name = item["name"].to_string();
        if ( item.contains("arguments")) {
            try { tc.arguments = JSON::parse(item["arguments"].to_string()); }
            catch ( ... ) { tc.arguments = JSON::Object{}; }
        }
        if ( !tc.id.empty()) _s_tools[tc.id] = tc;
    }
}

Response Codex::parse_response(const JSON& response) {
    stream_reset();
    if ( response.contains("output") && response["output"] == JSON::TYPE::ARRAY )
        for ( size_t i = 0; i < response["output"].size(); ++i ) capture_output_item(response["output"][i]);
    if ( response.contains("usage") && response["usage"] == JSON::TYPE::OBJECT ) {
        JSON u = response["usage"];
        if ( u.contains("input_tokens")) _s_input_tokens = json_long(u["input_tokens"]);
        if ( u.contains("output_tokens")) _s_output_tokens = json_long(u["output_tokens"]);
        if ( u.contains("input_tokens_details") && u["input_tokens_details"] == JSON::TYPE::OBJECT &&
             u["input_tokens_details"].contains("cached_tokens"))
            _s_cached_tokens = json_long(u["input_tokens_details"]["cached_tokens"]);
    }
    if ( response.contains("status") && response["status"].to_string() == "incomplete" ) _s_truncated = true;
    return stream_result();
}

JSON Codex::make_tool_result(const std::string& tool_call_id, const std::string& result) {
    return JSON::Object{{ "type", "function_call_output" }, { "call_id", tool_call_id }, { "output", result }};
}

void Codex::apply_provider_options(const JSON& options) {
    if ( options != JSON::TYPE::OBJECT ) return;
    if ( options.contains("model") && options["model"] == JSON::TYPE::STRING ) _config.model = options["model"].to_string();
    if ( options.contains("thinking") && options["thinking"] == JSON::TYPE::STRING ) {
        std::string v;
        for ( char c : options["thinking"].to_string()) v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if ( v == "off" || v == "false" || v == "disabled" || v == "0" ) _reasoning_enabled = false;
        else {
            _reasoning_enabled = true;
            _reasoning_effort = (v == "on" || v == "true" || v.empty()) ? "medium" : v;
            if ( _reasoning_effort == "max" ) _reasoning_effort = "xhigh";
        }
    }
}

void Codex::stream_reset() {
    _s_content.clear(); _s_reasoning.clear(); _s_tools.clear();
    _s_input_tokens = _s_output_tokens = _s_cached_tokens = 0;
    _s_truncated = false;
}

StreamChunk Codex::parse_stream(const std::string& chunk, std::string& buffer, bool& done) {
    for ( char c : chunk ) if ( c != '\r' ) buffer += c;
    StreamChunk out;
    size_t pos;
    while ((pos = buffer.find("\n\n")) != std::string::npos ) {
        std::string frame = buffer.substr(0, pos); buffer.erase(0, pos + 2);
        size_t data_pos = frame.find("data:");
        if ( data_pos == std::string::npos ) continue;
        std::string data = frame.substr(data_pos + 5);
        size_t first = data.find_first_not_of(" \t"); data = first == std::string::npos ? "" : data.substr(first);
        if ( data == "[DONE]" ) { done = true; continue; }
        try {
            JSON e = JSON::parse(data);
            if ( !e.contains("type")) continue;
            std::string type = e["type"].to_string();
            if ( type == "response.output_text.delta" && e.contains("delta")) {
                std::string delta = e["delta"].to_string();
                out.content += delta;
                _s_content += delta;
            } else if (( type == "response.reasoning_summary_text.delta" || type == "response.reasoning_text.delta" ) && e.contains("delta")) {
                std::string delta = e["delta"].to_string();
                out.reasoning += delta;
                _s_reasoning += delta;
            } else if ( type == "response.output_item.done" && e.contains("item")) {
                JSON item = e["item"];
                // Text was already accumulated via deltas; only capture calls.
                if ( item.contains("type") && item["type"].to_string() == "function_call" ) capture_output_item(item);
            } else if ( type == "response.completed" && e.contains("response")) {
                JSON r = e["response"];
                if ( r.contains("usage") && r["usage"] == JSON::TYPE::OBJECT ) {
                    JSON u = r["usage"];
                    if ( u.contains("input_tokens")) _s_input_tokens = json_long(u["input_tokens"]);
                    if ( u.contains("output_tokens")) _s_output_tokens = json_long(u["output_tokens"]);
                    if ( u.contains("input_tokens_details") && u["input_tokens_details"] == JSON::TYPE::OBJECT && u["input_tokens_details"].contains("cached_tokens"))
                        _s_cached_tokens = json_long(u["input_tokens_details"]["cached_tokens"]);
                }
                done = true;
            } else if ( type == "response.incomplete" ) { _s_truncated = true; done = true; }
            else if ( type == "response.failed" ) done = true;
        } catch ( ... ) {}
    }
    return out;
}

Response Codex::stream_result() {
    Response r;
    r.message = _s_content; r.thinking = _s_reasoning;
    r.input_tokens = _s_input_tokens; r.output_tokens = _s_output_tokens;
    r.cached_input_tokens = _s_cached_tokens; r.truncated = _s_truncated;
    for ( const auto& [id, tc] : _s_tools ) r.tool_calls.push_back(tc);
    return r;
}

} // namespace agent::providers
