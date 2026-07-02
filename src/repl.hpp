#pragma once

#include <string>
#include <memory>
#include "config.hpp"
#include "api/client.hpp"
#include "conversation.hpp"
#include "providers/provider.hpp"
#include "tools/registry.hpp"

namespace agent {

class Repl {
public:
    explicit Repl(const Config& config);

    void run();
    void run_once(const std::string& prompt);
    std::string process_turn(const std::string& prompt);

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
