#include "agent/conversation.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include "logger.hpp"
#include "throws.hpp"

namespace agent {

static std::string role_to_string(Role role) {
    switch ( role ) {
        case Role::SYSTEM: return "system";
        case Role::USER: return "user";
        case Role::ASSISTANT: return "assistant";
        case Role::TOOL: return "tool";
    }
    return "user";
}

static Role string_to_role(const std::string& s) {
    if ( s == "system" ) return Role::SYSTEM;
    if ( s == "assistant" ) return Role::ASSISTANT;
    if ( s == "tool" ) return Role::TOOL;
    return Role::USER;
}

void Conversation::set_system(const std::string& prompt) {
    // replace existing system message or prepend
    for ( auto& m : _messages ) {
        if ( m.role == Role::SYSTEM ) {
            m.content = prompt;
            return;
        }
    }
    _messages.insert(_messages.begin(), Message(Role::SYSTEM, prompt));
}

void Conversation::add_user(const std::string& content) {
    _messages.emplace_back(Role::USER, content);
}

void Conversation::add_assistant(const std::string& content) {
    _messages.emplace_back(Role::ASSISTANT, content);
}

void Conversation::add_assistant(const std::string& content, const std::vector<ToolCall>& tool_calls) {
    Message msg(Role::ASSISTANT, content);
    msg.tool_calls = tool_calls;
    _messages.push_back(std::move(msg));
}

void Conversation::add_tool_result(const std::string& tool_call_id, const std::string& name, const std::string& result) {
    _messages.emplace_back(Role::TOOL, result, tool_call_id, name);
}

void Conversation::clear() {
    _messages.clear();
}

std::vector<Message> Conversation::within_token_budget(size_t max_tokens) const {
    if ( max_tokens == 0 || _messages.empty())
        return _messages;

    auto est = [](const Message& m) -> size_t {
        size_t chars = m.content.size();
        for ( const auto& tc : m.tool_calls )
            chars += tc.arguments.size() + tc.name.size();
        return chars / 4 + 8; // rough per-message overhead
    };

    std::vector<Message> head;
    size_t budget = max_tokens;
    size_t start = 0;
    if ( _messages[0].role == Role::SYSTEM ) {
        head.push_back(_messages[0]);
        size_t s = est(_messages[0]);
        budget = ( s < budget ) ? budget - s : 0;
        start = 1;
    }

    // Accumulate from the newest backwards until the budget is exhausted (always
    // keep at least the most recent message so a turn can still be sent).
    std::vector<Message> tail;
    size_t used = 0;
    for ( size_t i = _messages.size(); i-- > start; ) {
        size_t s = est(_messages[i]);
        if ( used + s > budget && !tail.empty())
            break;
        used += s;
        tail.push_back(_messages[i]);
    }
    std::reverse(tail.begin(), tail.end());

    // A tool result whose assistant tool_call was trimmed away would be orphaned.
    while ( !tail.empty() && tail.front().role == Role::TOOL )
        tail.erase(tail.begin());

    std::vector<Message> out = std::move(head);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

std::string Conversation::undo_last() {
    for ( size_t i = _messages.size(); i-- > 0; ) {
        if ( _messages[i].role == Role::USER ) {
            std::string content = _messages[i].content;
            _messages.erase(_messages.begin() + i, _messages.end());
            return content;
        }
    }
    return "";
}

void Conversation::save(const std::string& path) const {

    JSON arr = JSON::Array{};
    for ( const auto& msg : _messages ) {
        JSON obj = JSON::Object{
            { "role", role_to_string(msg.role) },
            { "content", msg.content }
        };
        if ( msg.tool_call_id.has_value())
            obj["tool_call_id"] = msg.tool_call_id.value();
        if ( msg.name.has_value())
            obj["name"] = msg.name.value();
        if ( !msg.tool_calls.empty()) {
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
        arr.append(obj);
    }

    std::ofstream ofd(path, std::ios::out | std::ios::trunc);
    if ( !ofd.is_open())
        throws << "cannot open conversation file for writing: " << path << std::endl;

    ofd << arr.dump();
    logger::verbose["conversation"] << "saved " << _messages.size() << " message(s) to " << path << std::endl;
}

void Conversation::load(const std::string& path) {

    if ( !std::filesystem::exists(path))
        return;

    std::ifstream ifd(path, std::ios::in);
    if ( !ifd.is_open()) {
        logger::warning["conversation"] << "cannot open conversation file: " << path << std::endl;
        return;
    }

    std::stringstream ss;
    ss << ifd.rdbuf();

    try {
        JSON arr = JSON::parse(ss.str());
        if ( arr != JSON::TYPE::ARRAY ) {
            logger::warning["conversation"] << "conversation file is not an array, ignoring: " << path << std::endl;
            return;
        }

        std::vector<Message> loaded;
        for ( size_t i = 0; i < arr.size(); i++ ) {
            JSON obj = arr[i];
            Message msg;
            if ( obj.contains("role"))
                msg.role = string_to_role(obj["role"].to_string());
            if ( obj.contains("content"))
                msg.content = obj["content"].to_string();
            if ( obj.contains("tool_call_id"))
                msg.tool_call_id = obj["tool_call_id"].to_string();
            if ( obj.contains("name"))
                msg.name = obj["name"].to_string();
            if ( obj.contains("tool_calls") && obj["tool_calls"] == JSON::TYPE::ARRAY ) {
                JSON calls = obj["tool_calls"];
                for ( size_t i = 0; i < calls.size(); ++i ) {
                    JSON tc = calls[i];
                    ToolCall call;
                    if ( tc.contains("id"))
                        call.id = tc["id"].to_string();
                    if ( tc.contains("function") && tc["function"] == JSON::TYPE::OBJECT ) {
                        JSON fn = tc["function"];
                        if ( fn.contains("name"))
                            call.name = fn["name"].to_string();
                        if ( fn.contains("arguments"))
                            call.arguments = fn["arguments"].to_string();
                    }
                    msg.tool_calls.push_back(std::move(call));
                }
            }
            loaded.push_back(msg);
        }

        // Only replace the in-memory history once parsing fully succeeded.
        _messages = std::move(loaded);
        logger::verbose["conversation"] << "loaded " << _messages.size() << " message(s) from " << path << std::endl;
    } catch ( const std::exception& e ) {
        // A corrupt history must not crash the agent — warn and start fresh.
        logger::warning["conversation"] << "ignoring unreadable conversation file " << path << ": " << e.what() << std::endl;
    }
}

} // namespace agent
