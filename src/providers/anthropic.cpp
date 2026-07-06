#include "agent/providers/anthropic.hpp"

#include <cctype>
#include "throws.hpp"
#include "logger.hpp"

namespace agent::providers {

long Anthropic::thinking_budget_for(const std::string& effort, const std::string& model) {
    std::string m;
    for ( char c : model ) m += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    long cap = ( m.find("sonnet") != std::string::npos ) ? 64000 : 32000; // output ceiling
    long margin = 8192;        // leave room for the visible answer
    long maxb = cap - margin;

    long b;
    if ( effort == "low" ) b = 2048;
    else if ( effort == "medium" ) b = 8192;
    else if ( effort == "high" ) b = 16000;
    else if ( effort == "xhigh" ) b = 24000;
    else if ( effort == "max" ) b = maxb;   // Claude-specific: the model's ceiling
    else b = 8192;                          // plain "on" / enabled default
    if ( b > maxb ) b = maxb;
    if ( b < 1024 ) b = 1024;
    return b;
}

void Anthropic::apply_provider_options(const JSON& options) {
    if ( options.contains("thinking") && options["thinking"] == JSON::TYPE::STRING ) {
        std::string v = options["thinking"].to_string();
        std::string lower;
        for ( char c : v ) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if ( lower == "off" || lower == "false" || lower == "disabled" || lower == "0" ) {
            _thinking_enabled = false;
            _thinking_effort.clear();
        } else if ( lower == "on" || lower == "true" || lower == "enabled" || lower == "1" || lower.empty()) {
            _thinking_enabled = true;
            _thinking_effort.clear();
        } else {
            _thinking_enabled = true;
            _thinking_effort = lower;
        }
    }
}

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
        case agent::Role::TOOL: return "user"; // anthropic tool results go as user messages
    }
    return "user";
}

JSON Anthropic::message_to_json(const Message& msg) {
    // Assistant messages that contain tool calls must be sent as content blocks
    // so the model can correlate tool results with the original tool_use IDs.
    if ( msg.role == agent::Role::ASSISTANT && !msg.tool_calls.empty()) {
        JSON blocks = JSON::Array{};
        if ( !msg.content.empty()) {
            blocks.append(JSON::Object{
                { "type", "text" },
                { "text", msg.content }
            });
        }
        for ( const auto& tc : msg.tool_calls ) {
            JSON input = JSON::Object{};
            try {
                if ( !tc.arguments.empty())
                    input = JSON::parse(tc.arguments);
            } catch ( const std::exception& ) {
                input = JSON::Object{}; // tolerate empty / partial saved arguments
            }
            blocks.append(JSON::Object{
                { "type", "tool_use" },
                { "id", tc.id },
                { "name", tc.name },
                { "input", input }
            });
        }
        return JSON::Object{
            { "role", role_to_string(msg.role) },
            { "content", blocks }
        };
    }

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

    // Anthropic requires user/assistant roles to alternate. Merge a plain-text
    // message into the previous one when they share a role and both carry string
    // content — e.g. a /btw note directly followed by the next user prompt would
    // otherwise be two user turns in a row and get rejected.
    auto push = [&messages](const JSON& entry) {
        if ( messages.size() > 0 ) {
            JSON& last = messages[messages.size() - 1];
            if ( last["role"].to_string() == entry["role"].to_string()
                 && last["content"] == JSON::TYPE::STRING
                 && entry["content"] == JSON::TYPE::STRING ) {
                last["content"] = last["content"].to_string() + "\n\n" + entry["content"].to_string();
                return;
            }
        }
        messages.append(entry);
    };

    for ( const auto& msg : request_messages(conv)) {
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
            push(JSON::Object{
                { "role", "user" },
                { "content", content }
            });
        } else {
            push(message_to_json(msg));
        }
    }

    JSON req = JSON::Object{
        { "model", _config.model },
        { "max_tokens", 4096 },
        { "messages", messages }
    };

    // Extended thinking: budget_tokens must be < max_tokens, so raise max_tokens
    // above the budget. Temperature is left unset (Anthropic requires it to be 1
    // with thinking; unset defaults to 1).
    if ( _thinking_enabled ) {
        long budget = thinking_budget_for(_thinking_effort, _config.model);
        req["thinking"] = JSON::Object{
            { "type", "enabled" },
            { "budget_tokens", static_cast<long long>(budget) }
        };
        req["max_tokens"] = static_cast<long long>(budget + 8192);
    }

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
            } else if ( type == "thinking" && block.contains("thinking")) {
                r.thinking += block["thinking"].to_string();
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

    if ( response.contains("usage") && response["usage"] == JSON::TYPE::OBJECT ) {
        JSON u = response["usage"];
        if ( u.contains("input_tokens")) r.input_tokens = json_long(u["input_tokens"]);
        if ( u.contains("output_tokens")) r.output_tokens = json_long(u["output_tokens"]);
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

void Anthropic::stream_reset() {
    _s_content.clear();
    _s_reasoning.clear();
    _s_blocks.clear();
    _s_input_tokens = 0;
    _s_output_tokens = 0;
}

StreamChunk Anthropic::parse_stream(const std::string& chunk, std::string& buffer, bool& done) {
    // Strip CR so CRLF line endings still split on "\n\n" (some gateways send \r\n).
    for ( char ch : chunk )
        if ( ch != '\r' ) buffer += ch;
    StreamChunk out;
    size_t pos;
    while ((pos = buffer.find("\n\n")) != std::string::npos) {
        std::string frame = buffer.substr(0, pos);
        buffer.erase(0, pos + 2);

        if ( frame.find("event: message_stop") != std::string::npos )
            done = true;

        size_t data_pos = frame.find("data:");
        if ( data_pos == std::string::npos )
            continue;
        std::string data = frame.substr(data_pos + 5);
        size_t nsp = data.find_first_not_of(" \t");
        data = ( nsp == std::string::npos ) ? "" : data.substr(nsp);
        try {
            JSON j = JSON::parse(data);
            std::string type = j.contains("type") ? j["type"].to_string() : "";

            if ( type == "message_start" && j.contains("message") && j["message"].contains("usage")) {
                JSON u = j["message"]["usage"];
                if ( u == JSON::TYPE::OBJECT && u.contains("input_tokens"))
                    _s_input_tokens = json_long(u["input_tokens"]);
            } else if ( type == "content_block_start" && j.contains("index") && j.contains("content_block")) {
                int idx = static_cast<int>(json_long(j["index"]));
                JSON b = j["content_block"];
                StreamBlock& blk = _s_blocks[idx];
                blk.type = b.contains("type") ? b["type"].to_string() : "";
                if ( b.contains("id")) blk.id = b["id"].to_string();
                if ( b.contains("name")) blk.name = b["name"].to_string();
            } else if ( type == "content_block_delta" && j.contains("index") && j.contains("delta")) {
                int idx = static_cast<int>(json_long(j["index"]));
                JSON d = j["delta"];
                std::string dt = d.contains("type") ? d["type"].to_string() : "";
                if ( dt == "text_delta" && d.contains("text")) {
                    std::string t = d["text"].to_string();
                    _s_content += t;
                    out.content += t;
                } else if ( dt == "thinking_delta" && d.contains("thinking")) {
                    std::string t = d["thinking"].to_string();
                    _s_reasoning += t;
                    out.reasoning += t;
                } else if ( dt == "input_json_delta" && d.contains("partial_json")) {
                    _s_blocks[idx].json += d["partial_json"].to_string();
                }
            } else if ( type == "message_delta" && j.contains("usage")) {
                JSON u = j["usage"];
                if ( u.contains("output_tokens")) _s_output_tokens = json_long(u["output_tokens"]);
            }
        } catch ( const std::exception& e ) {
            // ignore malformed chunks
        }
    }
    return out;
}

Response Anthropic::stream_result() {
    Response r;
    r.message = _s_content;
    r.thinking = _s_reasoning;
    r.input_tokens = _s_input_tokens;
    r.output_tokens = _s_output_tokens;
    for ( const auto& [idx, blk] : _s_blocks ) {
        if ( blk.type != "tool_use" || blk.name.empty())
            continue;
        ToolCall call;
        call.id = blk.id;
        call.name = blk.name;
        try {
            call.arguments = blk.json.empty() ? JSON::Object{} : JSON::parse(blk.json);
        } catch ( const std::exception& ) {
            call.arguments = JSON::Object{};
        }
        r.tool_calls.push_back(call);
    }
    return r;
}

} // namespace agent::providers
