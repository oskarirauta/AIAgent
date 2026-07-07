#pragma once

#include <string>
#include <vector>
#include <optional>
#include "json.hpp"

namespace agent {

enum class Role {
    SYSTEM,
    USER,
    ASSISTANT,
    TOOL
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments; // JSON string
};

struct Message {
    Role role = Role::USER;
    std::string content;
    std::optional<std::string> tool_call_id;
    std::optional<std::string> name; // for tool result sender
    std::vector<ToolCall> tool_calls; // assistant tool calls

    // Raw Anthropic thinking/redacted_thinking blocks (with signatures), kept
    // verbatim: with extended thinking + tool use, the API requires them to be
    // replayed at the start of the assistant turn's content.
    JSON thinking_blocks = JSON::Array{};

    Message() = default;
    Message(Role r, const std::string& c) : role(r), content(c) {}
    Message(Role r, const std::string& c, const std::string& tcid, const std::string& n)
        : role(r), content(c), tool_call_id(tcid), name(n) {}
};

class Conversation {
public:
    void set_system(const std::string& prompt);
    void add_user(const std::string& content);
    void add_assistant(const std::string& content);
    void add_assistant(const std::string& content, const std::vector<ToolCall>& tool_calls,
                       const JSON& thinking_blocks = JSON::Array{});
    void add_tool_result(const std::string& tool_call_id, const std::string& name, const std::string& result);

    const std::vector<Message>& messages() const { return _messages; }
    void clear();

    // Messages to send under an approximate token budget (4 chars ≈ 1 token):
    // keep a leading system message plus the most recent messages that fit.
    // 0 means no limit (returns the full history). Leading orphaned tool
    // results — whose assistant tool_call got trimmed — are dropped.
    std::vector<Message> within_token_budget(size_t max_tokens) const;

    // Remove the most recent exchange: everything from the last user message to
    // the end (its assistant reply and any tool messages). Returns the removed
    // user message's content, or empty if there was nothing to undo.
    std::string undo_last();

    // Elide the body of a tool result made stale by a LATER result for the same
    // target (same file path for read/write/edit/outline; same run_command
    // string), keeping the pairing intact. A pure transform on a message list —
    // applied per request after trimming. The newest result for each target is
    // kept in full.
    static std::vector<Message> supersede_stale_tools(std::vector<Message> msgs);

    void save(const std::string& path) const;
    void load(const std::string& path);

private:
    std::vector<Message> _messages;

    // Hysteresis trim boundary: the index of the first non-system message to
    // include. within_token_budget pins it so the request PREFIX stays byte-
    // identical across turns (prompt-cache friendly) until the budget is truly
    // exceeded again. `mutable` because it is a cache, not logical state. Reset
    // by clear(). 0 means "no cut pinned yet".
    mutable size_t _trim_start = 0;
};

} // namespace agent
