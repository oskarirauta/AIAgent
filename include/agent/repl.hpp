#pragma once

#include <string>
#include <memory>
#include <atomic>
#include "agent/config.hpp"
#include "agent/api/client.hpp"
#include "agent/conversation.hpp"
#include "agent/providers/provider.hpp"
#include "agent/tools/registry.hpp"
#include "agent/token_stats.hpp"

namespace agent {

class Repl {
public:
    explicit Repl(const Config& config);

    void run();
    void run_once(const std::string& prompt);
    std::string process_turn(const std::string& prompt, std::function<void(const std::string&)> stream_cb = nullptr, std::atomic<bool>* abort_flag = nullptr);
    void save_conversation();

    // Query whether the active provider supports a provider-specific capability
    // (e.g. "model-command"). Returns false if no provider is loaded.
    bool provider_supports(const std::string& capability) const {
        return _provider && _provider->supports(capability);
    }

private:
    void run_tty();
    void run_plain();
    std::string conversation_path() const;
    tools::ConfirmMode tool_mode() const;

    // Handle a slash command (e.g. /settings, /model). Returns text to display.
    std::string handle_command(const std::string& line);

    // Rebuild the system prompt (config + current date + memories).
    std::string base_system_prompt() const;
    // Summarise the conversation via one LLM call and replace history with it.
    std::string compact_history();
    // Switch the active provider mid-session, carrying the current conversation
    // over (re-auth non-interactively). Returns text to display.
    std::string switch_provider(const std::string& name);

    Config _config;
    api::Client _client;
    tools::Registry _registry;
    std::unique_ptr<providers::Provider> _provider;
    Conversation _conversation;
    TokenStats _stats;
};

} // namespace agent
