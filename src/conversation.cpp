#include "conversation.hpp"

namespace agent {

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

void Conversation::add_tool_result(const std::string& tool_call_id, const std::string& name, const std::string& result) {
    _messages.emplace_back(Role::TOOL, result, tool_call_id, name);
}

void Conversation::clear() {
    _messages.clear();
}

} // namespace agent
