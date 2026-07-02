#include "agent/repl.hpp"

#include <iostream>
#include <unistd.h>
#include "json.hpp"
#include "logger.hpp"
#include "common.hpp"
#include "throws.hpp"
#include "agent/repl_ncurses.hpp"
#include "agent/signal_handler.hpp"
#include "agent/memory.hpp"

namespace agent {

Repl::Repl(const Config& config)
    : _config(config), _provider(providers::create(config)) {

    if ( config.tools_enabled )
        _registry.register_defaults();
    else
        logger::info["agent"] << "tools disabled" << std::endl;

    std::string system = config.system_prompt;
    std::string memories = load_memories(config.home_dir);
    if ( !memories.empty())
        system += memories;

    _conversation.set_system(system);
    _conversation.load(config.home_dir + "/conversations/default.json");
}

void Repl::save_conversation() {
    std::string dir = _config.home_dir + "/conversations";
    if ( !std::filesystem::exists(dir))
        std::filesystem::create_directories(dir);
    _conversation.save(dir + "/default.json");
}

std::string Repl::process_turn(const std::string& prompt, std::function<void(const std::string&)> stream_cb) {

    _conversation.add_user(prompt);

    while ( true ) {

        JSON tools = _registry.schema();
        JSON request = _provider->build_request(_conversation, tools);

        // Use streaming when supported, requested, and no tools are registered
        bool can_stream = stream_cb && _provider->supports_streaming() &&
                          ( tools != JSON::TYPE::ARRAY || tools.empty());

        if ( can_stream ) {
            request["stream"] = true;
            std::string body = request.dump_minified();
            std::string buffer;
            bool done = false;
            std::string full_reply;

            _client.post_stream(_provider->endpoint(), _provider->auth_header(), _provider->auth_value(), body,
                [&](const std::string& chunk) {
                    std::string text = _provider->parse_stream(chunk, buffer, done);
                    if ( !text.empty()) {
                        full_reply += text;
                        stream_cb(text);
                    }
                });

            _conversation.add_assistant(full_reply);
            return full_reply;
        }

        std::string body = request.dump_minified();
        std::string response_str = _client.post(_provider->endpoint(), _provider->auth_header(), _provider->auth_value(), body);
        JSON response = JSON::parse(response_str);
        providers::Response resp = _provider->parse_response(response);

        if ( !resp.success )
            throws << "provider response error: " << resp.message << std::endl;

        _conversation.add_assistant(resp.message);

        if ( resp.tool_calls.empty())
            return resp.message;

        for ( const auto& tc : resp.tool_calls ) {

            std::string result;
            try {
                result = _registry.execute(tc.name, tc.arguments);
            } catch ( const std::exception& e ) {
                result = std::string("error: ") + e.what();
            }

            logger::info["tool"] << tc.name << " -> " << result.substr(0, 200) << std::endl;
            _conversation.add_tool_result(tc.id, tc.name, result);
        }

        // loop back to send tool results to model
    }
}

void Repl::run_plain() {

    std::cout << "AI Agent ready. Type 'exit' or 'quit' to leave.\n" << std::endl;

    if ( _config.confirm_tools ) {
        _registry.set_confirm_callback([](const std::string& action) -> bool {
            std::cout << "\nAllow tool: " << action << " [y/N]? " << std::flush;
            std::string answer;
            if ( !std::getline(std::cin, answer))
                return false;
            std::string a = common::to_lower(common::trim_ws(answer));
            return a == "y" || a == "yes";
        });
    }

    std::string line;
    while ( agent::running.load(std::memory_order_relaxed)) {
        std::cout << "> " << std::flush;
        if ( !std::getline(std::cin, line))
            break;

        if ( common::trim_ws(line).empty())
            continue;

        if ( line == "exit" || line == "quit" )
            break;

        try {
            std::string reply = process_turn(line, [](const std::string& chunk) {
                std::cout << chunk << std::flush;
            });
            std::cout << std::endl;
            save_conversation();
        } catch ( const std::exception& e ) {
            logger::error << "request failed: " << e.what() << std::endl;
        }

        std::cout << std::endl;
    }
}

void Repl::run_tty() {

    if ( _config.confirm_tools ) {
        // Ncurses mode cannot easily prompt for confirmation inline, so decline destructive tools.
        _registry.set_confirm_callback([](const std::string&) { return false; });
    }

    NcursesRepl ncurses(
        [this](const std::string& prompt, std::function<void(const std::string&)> stream_cb) -> std::string {
            try {
                std::string reply = this->process_turn(prompt, stream_cb);
                this->save_conversation();
                return reply;
            } catch ( const std::exception& e ) {
                return std::string("error: ") + e.what();
            }
        },
        _config, _conversation);

    ncurses.run();
}

void Repl::run() {

    if ( isatty(STDIN_FILENO))
        run_tty();
    else
        run_plain();
}

void Repl::run_once(const std::string& prompt) {

    try {
        std::string reply = process_turn(prompt, [](const std::string& chunk) {
            std::cout << chunk << std::flush;
        });
        std::cout << std::endl;
        save_conversation();
    } catch ( const std::exception& e ) {
        logger::error << "request failed: " << e.what() << std::endl;
    }
}

} // namespace agent
