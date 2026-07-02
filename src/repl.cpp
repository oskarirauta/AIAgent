#include "repl.hpp"

#include <iostream>
#include <unistd.h>
#include "json.hpp"
#include "logger.hpp"
#include "common.hpp"
#include "throws.hpp"
#include "repl_ncurses.hpp"

namespace agent {

Repl::Repl(const Config& config)
    : _config(config), _provider(providers::create(config)) {

    _registry.register_defaults();
    _conversation.set_system(config.system_prompt);
}

std::string Repl::process_turn(const std::string& prompt) {

    _conversation.add_user(prompt);

    while ( true ) {

        JSON tools = _registry.schema();
        JSON request = _provider->build_request(_conversation, tools);
        std::string body = request.dump_minified();

        std::string response_str = _client.post(_provider->endpoint(), _config.api_key, body);
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

    std::string line;
    while ( true ) {
        std::cout << "> " << std::flush;
        if ( !std::getline(std::cin, line))
            break;

        if ( common::trim_ws(line).empty())
            continue;

        if ( line == "exit" || line == "quit" )
            break;

        try {
            std::string reply = process_turn(line);
            std::cout << reply << std::endl;
        } catch ( const std::exception& e ) {
            logger::error << "request failed: " << e.what() << std::endl;
        }

        std::cout << std::endl;
    }
}

void Repl::run_tty() {

    NcursesRepl ncurses([this](const std::string& prompt) -> std::string {
        try {
            return this->process_turn(prompt);
        } catch ( const std::exception& e ) {
            return std::string("error: ") + e.what();
        }
    });

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
        std::string reply = process_turn(prompt);
        std::cout << reply << std::endl;
    } catch ( const std::exception& e ) {
        logger::error << "request failed: " << e.what() << std::endl;
    }
}

} // namespace agent
