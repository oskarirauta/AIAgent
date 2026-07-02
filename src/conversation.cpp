#include "agent/conversation.hpp"

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

void Conversation::add_tool_result(const std::string& tool_call_id, const std::string& name, const std::string& result) {
    _messages.emplace_back(Role::TOOL, result, tool_call_id, name);
}

void Conversation::clear() {
    _messages.clear();
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
        if ( arr != JSON::TYPE::ARRAY )
            throws << "conversation file is not an array: " << path << std::endl;

        _messages.clear();
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
            _messages.push_back(msg);
        }

        logger::verbose["conversation"] << "loaded " << _messages.size() << " message(s) from " << path << std::endl;
    } catch ( const std::exception& e ) {
        throws << "failed to load conversation: " << e.what() << std::endl;
    }
}

} // namespace agent
