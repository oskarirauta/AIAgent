#include "agent/conversation.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include "logger.hpp"
#include "throws.hpp"
#include "common.hpp"

namespace agent {

namespace {

// The "target" a tool call acts on, for supersession. Empty = never superseded.
// File tools key on their path; run_command keys on its command string;
// list_directory, grep, find_symbol, and find_references key on their targets.
// A later call with the same target/query makes the earlier result stale.
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
    if ( tool == "list_directory" ) {
        std::string p = args.contains("path") ? common::trim_ws(args["path"].to_string()) : ".";
        return "dir:" + p;
    }
    if ( tool == "grep" && args.contains("pattern")) {
        std::string p = args.contains("path") ? common::trim_ws(args["path"].to_string()) : ".";
        return "grep:" + p + ":" + common::trim_ws(args["pattern"].to_string());
    }
    if ( tool == "find_symbol" && args.contains("name"))
        return "sym:" + common::trim_ws(args["name"].to_string());
    if ( tool == "find_references" && args.contains("name"))
        return "ref:" + common::trim_ws(args["name"].to_string());
    return "";
}

std::string supersede_describe(const std::string& key) {
    if ( key.rfind("file:", 0) == 0 )
        return "later access to " + key.substr(5);
    if ( key.rfind("cmd:", 0) == 0 )
        return "a later run of `" + key.substr(4) + "`";
    if ( key.rfind("dir:", 0) == 0 )
        return "a later listing of " + key.substr(4);
    if ( key.rfind("grep:", 0) == 0 )
        return "a later grep in " + key.substr(5);
    if ( key.rfind("sym:", 0) == 0 )
        return "a later lookup of " + key.substr(4);
    if ( key.rfind("ref:", 0) == 0 )
        return "a later reference search for " + key.substr(4);
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

size_t Conversation::estimate_message_tokens(const Message& m, const std::string& provider) {
    std::string prov = common::to_lower(common::trim_ws(provider));

    double chars_per_tok = 4.0;
    size_t overhead = 8;
    if ( prov == "gemini" ) {
        chars_per_tok = 3.5;
        overhead = 6;
    } else if ( prov == "claude" || prov == "anthropic" ) {
        chars_per_tok = 3.8;
        overhead = 6;
    } else if ( prov == "openai" || prov == "codex" ) {
        chars_per_tok = 4.0;
        overhead = 5;
    }

    size_t chars = m.content.size();
    for ( const auto& tc : m.tool_calls )
        chars += tc.arguments.size() + tc.name.size();
    return static_cast<size_t>(chars / chars_per_tok) + overhead;
}

size_t Conversation::estimate_text_tokens(const std::string& text, const std::string& provider) {
    std::string prov = common::to_lower(common::trim_ws(provider));
    double chars_per_tok = 4.0;
    if ( prov == "gemini" ) chars_per_tok = 3.5;
    else if ( prov == "claude" || prov == "anthropic" ) chars_per_tok = 3.8;
    else if ( prov == "openai" || prov == "codex" ) chars_per_tok = 4.0;
    return static_cast<size_t>(text.size() / chars_per_tok);
}

size_t Conversation::estimate_tokens(const std::string& provider) const {
    size_t total = 0;
    for ( const auto& m : _messages )
        total += estimate_message_tokens(m, provider);
    return total;
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

std::vector<Message> Conversation::elide_old_large_tool_results(std::vector<Message> msgs) {
    constexpr size_t LARGE_TOOL_RESULT_CHARS = 2500;
    constexpr size_t KEEP_RECENT_TOOL_RESULTS = 2;

    std::vector<size_t> tool_indices;
    for ( size_t i = 0; i < msgs.size(); ++i )
        if ( msgs[i].role == Role::TOOL )
            tool_indices.push_back(i);

    if ( tool_indices.size() <= KEEP_RECENT_TOOL_RESULTS )
        return msgs;

    size_t keep_from = tool_indices.size() - KEEP_RECENT_TOOL_RESULTS;
    for ( size_t ti = 0; ti < keep_from; ++ti ) {
        Message& m = msgs[tool_indices[ti]];
        if ( m.content.size() <= LARGE_TOOL_RESULT_CHARS )
            continue;
        size_t total_bytes = m.content.size();
        size_t lines = static_cast<size_t>(std::count(m.content.begin(), m.content.end(), '\n')) + 1;
        std::string tool = m.name.value_or("tool");

        std::string excerpt;
        if ( lines <= 5 ) {
            excerpt = m.content.substr(0, 200);
            if ( m.content.size() > 200 ) excerpt += " …";
        } else {
            size_t p = 0;
            int count = 0;
            while ( count < 3 && p < m.content.size() ) {
                size_t nl = m.content.find('\n', p);
                if ( nl == std::string::npos ) { p = m.content.size(); break; }
                p = nl + 1;
                count++;
            }
            std::string head_snippet = m.content.substr(0, p);
            if ( !head_snippet.empty() && head_snippet.back() == '\n' ) head_snippet.pop_back();

            size_t tail_start = m.content.size();
            int tail_count = 0;
            while ( tail_count < 2 && tail_start > 0 ) {
                size_t prev_nl = m.content.rfind('\n', tail_start - 1);
                if ( prev_nl == std::string::npos ) { tail_start = 0; break; }
                tail_start = prev_nl;
                tail_count++;
            }
            if ( tail_start < m.content.size() && m.content[tail_start] == '\n' ) tail_start++;
            std::string tail_snippet = m.content.substr(tail_start);

            size_t elided_count = lines > 5 ? lines - 5 : 0;
            excerpt = head_snippet + "\n  ... [" + std::to_string(elided_count) + " lines elided] ...\n  " + tail_snippet;
        }

        m.content = "[older large tool result elided: " + tool + ", " +
                    std::to_string(total_bytes) + " bytes, " +
                    std::to_string(lines) + " lines; re-run tool if needed]\n  " +
                    excerpt;
    }
    return msgs;
}

std::vector<Message> Conversation::within_token_budget(size_t max_tokens, std::vector<Message> msgs, const std::string& provider) const {
    const bool custom_msgs = !msgs.empty();
    const std::vector<Message>& src = custom_msgs ? msgs : _messages;
    if ( max_tokens == 0 || src.empty()) {
        if ( !custom_msgs ) _trim_start = 0;
        return src;
    }

    auto est = [&provider](const Message& m) -> size_t {
        return estimate_message_tokens(m, provider);
    };

    std::vector<Message> head;
    size_t budget = max_tokens;
    size_t first = 0;
    if ( src[0].role == Role::SYSTEM ) {
        head.push_back(src[0]);
        size_t s = est(src[0]);
        budget = ( s < budget ) ? budget - s : 0;
        first = 1;
    }

    auto region_size = [&](size_t from) {
        size_t u = 0;
        for ( size_t i = from; i < src.size(); ++i )
            u += est(src[i]);
        return u;
    };

    // Keep the pinned cut valid against append/undo/compact.
    if ( _trim_start < first ) _trim_start = first;
    if ( _trim_start > src.size()) _trim_start = src.size();

    // Hysteresis: only move the cut when it is actually necessary. The pinned
    // region is kept within a [40%, 100%]-of-budget band so the cut (and thus
    // the request prefix) stays stable across many turns for prompt-cache hits.
    //  - If the WHOLE history fits, un-pin (include everything).
    //  - Otherwise re-cut to ~70% of budget when the pinned region left the band:
    //    it grew past budget (new turns), OR shrank well under target because
    //    undo/compact removed messages under the pin (a stale, too-high pin would
    //    otherwise drop even the most recent turns, degrading to system-only).
    if ( region_size(first) <= budget ) {
        _trim_start = first;
    } else {
        size_t pinned = region_size(_trim_start);
        if ( pinned > budget || pinned < budget * 4 / 10 ) {
            size_t target = budget * 7 / 10;
            size_t used = 0;
            size_t newstart = src.size();
            for ( size_t i = src.size(); i-- > first; ) {
                size_t s = est(src[i]);
                if ( used + s > target && newstart < src.size())
                    break; // keep at least the most recent message
                used += s;
                newstart = i;
            }
            _trim_start = newstart;
        }
    }

    // Snap the cut back to a USER turn: the first non-system message must be a
    // user message (Anthropic rejects a leading assistant/tool), and a tool_result
    // must never be separated from the assistant tool_call that produced it. This
    // subsumes the old "strip leading orphaned tool results" step.
    while ( _trim_start > first && _trim_start < src.size() &&
            src[_trim_start].role != Role::USER )
        --_trim_start;

    std::vector<Message> tail(src.begin() + _trim_start, src.end());

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

    // Write to a sibling temp file and rename it over the target only once the
    // write is KNOWN good. A straight overwrite truncates the target first, so a
    // full disk (or a crash mid-write) used to destroy the previous, intact
    // history — the only copy of the conversation.
    std::string tmp = path + ".tmp";
    {
        std::ofstream ofd(tmp, std::ios::out | std::ios::trunc);
        if ( !ofd.is_open())
            throws << "cannot open conversation file for writing: " << tmp << std::endl;
        ofd << arr.dump();
        ofd.flush();
        if ( !ofd.good()) {
            ofd.close();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            throws << "conversation save failed — disk full? (" << tmp << ")" << std::endl;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if ( ec ) {
        std::error_code ec2;
        std::filesystem::remove(tmp, ec2);
        throws << "conversation save could not replace " << path << ": " << ec.message() << std::endl;
    }
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

    // An unreadable history must not crash the agent — but it must not be left in
    // place to be OVERWRITTEN either: after an ignored load the conversation is
    // empty, and the first save would have replaced the damaged file (the only
    // copy, likely just truncated by a full disk) with the fresh session. Move it
    // aside so the bytes survive for recovery and the session starts cleanly.
    auto quarantine = [&path](const std::string& why) {
        std::string aside = path + ".corrupt-" + std::to_string(static_cast<long long>(::time(nullptr)));
        std::error_code ec;
        std::filesystem::rename(path, aside, ec);
        logger::warning["conversation"] << "conversation file " << path << " is unreadable (" << why
                                        << ") — moved to " << aside << ", starting fresh" << std::endl;
    };

    try {
        JSON arr = JSON::parse(ss.str());
        if ( arr != JSON::TYPE::ARRAY ) {
            quarantine("not a JSON array");
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
        quarantine(e.what());
    }
}

} // namespace agent
