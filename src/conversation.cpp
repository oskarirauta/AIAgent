#include "agent/conversation.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include "logger.hpp"
#include "throws.hpp"
#include "common.hpp"

namespace agent {

namespace {

// The "target" a tool call acts on, for supersession. Empty = never superseded.
// File tools key on their path; run_command keys on its command string, so a
// later read/write/edit of the same file (or re-run of the same command) makes
// an earlier result stale.
std::string supersede_key(const std::string& tool, const std::string& args_json) {
    JSON args;
    try { args = args_json.empty() ? JSON::Object{} : JSON::parse(args_json); }
    catch ( ... ) { return ""; }
    if ( args != JSON::TYPE::OBJECT )
        return "";
    if (( tool == "read_file" || tool == "write_file" || tool == "edit_file" ||
          tool == "outline_file" ) && args.contains("path"))
        return "file:" + common::trim_ws(args["path"].to_string());
    if ( tool == "run_command" && args.contains("command"))
        return "cmd:" + common::trim_ws(args["command"].to_string());
    return "";
}

std::string supersede_describe(const std::string& key) {
    if ( key.rfind("file:", 0) == 0 )
        return "later access to " + key.substr(5);
    if ( key.rfind("cmd:", 0) == 0 )
        return "a later run of `" + key.substr(4) + "`";
    return "a later action";
}

} // namespace

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

void Conversation::add_assistant(const std::string& content, const std::vector<ToolCall>& tool_calls,
                                 const JSON& thinking_blocks) {
    Message msg(Role::ASSISTANT, content);
    msg.tool_calls = tool_calls;
    if ( thinking_blocks == JSON::TYPE::ARRAY && thinking_blocks.size() > 0 )
        msg.thinking_blocks = thinking_blocks;
    _messages.push_back(std::move(msg));
}

void Conversation::add_tool_result(const std::string& tool_call_id, const std::string& name, const std::string& result) {
    _messages.emplace_back(Role::TOOL, result, tool_call_id, name);
}

void Conversation::clear() {
    _messages.clear();
    _trim_start = 0; // the pinned cut refers to the old history; drop it
}

std::vector<Message> Conversation::supersede_stale_tools(std::vector<Message> msgs) {
    // Map each tool_call_id to the target its call acts on (from the assistant
    // message that issued it).
    std::unordered_map<std::string, std::string> key_of;
    for ( const auto& m : msgs )
        if ( m.role == Role::ASSISTANT )
            for ( const auto& tc : m.tool_calls ) {
                std::string k = supersede_key(tc.name, tc.arguments);
                if ( !k.empty())
                    key_of[tc.id] = k;
            }

    // The last (newest) tool result index per target — that one is kept in full.
    std::unordered_map<std::string, size_t> last_idx;
    for ( size_t i = 0; i < msgs.size(); ++i )
        if ( msgs[i].role == Role::TOOL && msgs[i].tool_call_id.has_value()) {
            auto it = key_of.find(msgs[i].tool_call_id.value());
            if ( it != key_of.end())
                last_idx[it->second] = i;
        }

    // Elide the body of every earlier result whose target has a newer result.
    for ( size_t i = 0; i < msgs.size(); ++i ) {
        if ( msgs[i].role != Role::TOOL || !msgs[i].tool_call_id.has_value())
            continue;
        auto it = key_of.find(msgs[i].tool_call_id.value());
        if ( it == key_of.end())
            continue;
        auto last = last_idx.find(it->second);
        if ( last == last_idx.end() || last->second == i )
            continue; // this is the newest for its target — keep it
        if ( msgs[i].content.size() <= 120 )
            continue; // too small to bother eliding
        size_t lines = static_cast<size_t>(std::count(msgs[i].content.begin(), msgs[i].content.end(), '\n')) + 1;
        msgs[i].content = "[superseded by " + supersede_describe(it->second) + " — " +
                          std::to_string(lines) + " lines elided; re-run the tool if you need it again]";
    }
    return msgs;
}

std::vector<Message> Conversation::within_token_budget(size_t max_tokens) const {
    if ( max_tokens == 0 || _messages.empty()) {
        _trim_start = 0;
        return _messages;
    }

    auto est = [](const Message& m) -> size_t {
        size_t chars = m.content.size();
        for ( const auto& tc : m.tool_calls )
            chars += tc.arguments.size() + tc.name.size();
        return chars / 4 + 8; // rough per-message overhead
    };

    std::vector<Message> head;
    size_t budget = max_tokens;
    size_t first = 0;
    if ( _messages[0].role == Role::SYSTEM ) {
        head.push_back(_messages[0]);
        size_t s = est(_messages[0]);
        budget = ( s < budget ) ? budget - s : 0;
        first = 1;
    }

    auto region_size = [&](size_t from) {
        size_t u = 0;
        for ( size_t i = from; i < _messages.size(); ++i )
            u += est(_messages[i]);
        return u;
    };

    // Keep the pinned cut valid against append/undo/compact.
    if ( _trim_start < first ) _trim_start = first;
    if ( _trim_start > _messages.size()) _trim_start = _messages.size();

    // Hysteresis: only move the cut when it is actually necessary.
    //  - If the WHOLE history fits, un-pin (include everything).
    //  - If the pinned region has grown past the budget, re-cut to ~70% of the
    //    budget and re-pin — leaving headroom so the cut (and thus the request
    //    prefix) stays stable for many turns, giving prompt-cache hits.
    //  - Otherwise reuse the pinned cut unchanged.
    if ( region_size(first) <= budget ) {
        _trim_start = first;
    } else if ( region_size(_trim_start) > budget ) {
        size_t target = budget * 7 / 10;
        size_t used = 0;
        size_t newstart = _messages.size();
        for ( size_t i = _messages.size(); i-- > first; ) {
            size_t s = est(_messages[i]);
            if ( used + s > target && newstart < _messages.size())
                break; // keep at least the most recent message
            used += s;
            newstart = i;
        }
        _trim_start = newstart;
    }

    std::vector<Message> tail(_messages.begin() + _trim_start, _messages.end());

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
        if ( msg.thinking_blocks == JSON::TYPE::ARRAY && msg.thinking_blocks.size() > 0 )
            obj["thinking_blocks"] = msg.thinking_blocks;
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
            if ( obj.contains("thinking_blocks") && obj["thinking_blocks"] == JSON::TYPE::ARRAY )
                msg.thinking_blocks = obj["thinking_blocks"];
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
