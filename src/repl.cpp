#include "repl.hpp"

#include <iostream>
#include "json.hpp"
#include "logger.hpp"
#include "common.hpp"
#include "throws.hpp"

namespace agent {

Repl::Repl(const Config& config)
    : _config(config), _provider(providers::create(config)) {

    _registry.register_defaults();
    _conversation.set_system(config.system_prompt);
}

void Repl::process_turn() {

    JSON tools = _registry.schema();
    JSON request = _provider->build_request(_conversation, tools);
    std::string body = request.dump_minified();

    std::string response_str = _client.post(_provider->endpoint(), _config.api_key, body);
    JSON response = JSON::parse(response_str);
    providers::Response resp = _provider->parse_response(response);

    if ( !resp.success )
        throws << "provider response error: " << resp.message << std::endl;

    if ( resp.tool_calls.empty()) {
        _conversation.add_assistant(resp.message);
        std::cout << resp.message << std::endl;
        return;
    }

    // model decided to use tools
    _conversation.add_assistant(resp.message);

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

    // send results back to model
    process_turn();
}

void Repl::run() {

    std::cout << "AI Agent ready. Type 'exit' or 'quit' to leave.\n" << std::endl;

    std::string line;
    while ( true ) {
        std::cout << "> " << std::flush;
        if ( !std::getline(std::cin, line))
            break;

        if ( common::trimmed(line, common::whitespace).empty())
            continue;

        if ( line == "exit" || line == "quit" )
            break;

        _conversation.add_user(line);

        try {
            process_turn();
        } catch ( const std::exception& e ) {
            logger::error << "request failed: " << e.what() << std::endl;
        }

        std::cout << std::endl;
    }
}

void Repl::run_once(const std::string& prompt) {

    _conversation.add_user(prompt);

    try {
        process_turn();
    } catch ( const std::exception& e ) {
        logger::error << "request failed: " << e.what() << std::endl;
    }
}

} // namespace agent
