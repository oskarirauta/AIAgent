#pragma once

#include <string>
#include <memory>
#include "agent/config.hpp"
#include "agent/api/client.hpp"
#include "agent/conversation.hpp"
#include "agent/providers/provider.hpp"
#include "agent/tools/registry.hpp"

namespace agent {

class Repl {
public:
    explicit Repl(const Config& config);

    void run();
    void run_once(const std::string& prompt);
    std::string process_turn(const std::string& prompt, std::function<void(const std::string&)> stream_cb = nullptr);
    void save_conversation();

private:
    void run_tty();
    void run_plain();

    Config _config;
    api::Client _client;
    tools::Registry _registry;
    std::unique_ptr<providers::Provider> _provider;
    Conversation _conversation;
};

} // namespace agent
