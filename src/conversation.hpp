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

struct Message {
    Role role = Role::USER;
    std::string content;
    std::optional<std::string> tool_call_id;
    std::optional<std::string> name; // for tool result sender

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
    void add_tool_result(const std::string& tool_call_id, const std::string& name, const std::string& result);

    const std::vector<Message>& messages() const { return _messages; }
    void clear();

    void save(const std::string& path) const;
    void load(const std::string& path);

private:
    std::vector<Message> _messages;
};

} // namespace agent
