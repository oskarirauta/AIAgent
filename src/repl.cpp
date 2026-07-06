#include "agent/repl.hpp"

#include <iostream>
#include <unistd.h>
#include <cctype>
#include <filesystem>
#include "json.hpp"
#include "logger.hpp"
#include "common.hpp"
#include "throws.hpp"
#include "agent/repl_inline.hpp"
#include "agent/signal_handler.hpp"
#include "agent/memory.hpp"
#include "agent/text_utils.hpp"

namespace agent {

Repl::Repl(const Config& config)
    : _config(config), _provider(providers::create(config)) {

    if ( config.tools_enabled )
        _registry.register_defaults();
    else
        logger::info["agent"] << "tools disabled" << std::endl;

    // Load any persisted history for THIS provider first, then (re)apply the
    // current system prompt so a provider's identity and freshly-loaded memories
    // always reflect the running config rather than whatever was saved earlier.
    _conversation.load(conversation_path());

    std::string system = config.system_prompt;
    std::string memories = load_memories(config.home_dir, config.provider);
    if ( !memories.empty())
        system += memories;

    _conversation.set_system(system);
}

std::string Repl::conversation_path() const {
    // History is scoped by provider AND project (working directory), so different
    // projects keep separate chats and Claude's chat never bleeds into Kimi's.
    // The cwd is turned into a readable, filesystem-safe key, e.g.
    // /usr/src/AIAgent -> -usr-src-AIAgent (matching Claude Code's convention).
    std::string cwd;
    try {
        cwd = std::filesystem::current_path().string();
    } catch ( ... ) {
        cwd = "";
    }

    std::string key;
    for ( char c : cwd )
        key += std::isalnum(static_cast<unsigned char>(c)) ? c : '-';
    if ( key.empty())
        key = "default";

    return _config.home_dir + "/conversations/" + _config.provider + "/" + key + ".json";
}

void Repl::save_conversation() {
    std::string path = conversation_path();
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    _conversation.save(path);
}

std::string Repl::process_turn(const std::string& prompt, std::function<void(const std::string&)> stream_cb, std::atomic<bool>* abort_flag) {

    _conversation.add_user(prompt);

    while ( true ) {

        if ( abort_flag && abort_flag->load(std::memory_order_relaxed))
            return "";

        _provider->prepare_request(_client);

        auto headers = _provider->extra_headers();

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

            _client.post_stream(_provider->endpoint(), _provider->auth_header(), _provider->auth_value(), headers, body,
                [&](const std::string& chunk) {
                    std::string text = agent::normalize_text(_provider->parse_stream(chunk, buffer, done));
                    if ( !text.empty()) {
                        full_reply += text;
                        stream_cb(text);
                    }
                }, abort_flag);

            if ( abort_flag && abort_flag->load(std::memory_order_relaxed))
                return "";

            _conversation.add_assistant(full_reply);
            return full_reply;
        }

        std::string body = request.dump_minified();
        std::string response_str = _client.post(_provider->endpoint(), _provider->auth_header(), _provider->auth_value(), headers, body, abort_flag);

        if ( abort_flag && abort_flag->load(std::memory_order_relaxed))
            return "";

        JSON response = JSON::parse(response_str);
        providers::Response resp = _provider->parse_response(response);

        _stats.record(resp.input_tokens, resp.output_tokens);

        if ( !resp.success )
            throws << "provider response error: " << resp.message << std::endl;

        std::string normalized = agent::normalize_text(resp.message);

        std::vector<agent::ToolCall> assistant_calls;
        for ( const auto& tc : resp.tool_calls ) {
            assistant_calls.push_back({ tc.id, tc.name, tc.arguments.dump_minified() });
        }
        _conversation.add_assistant(normalized, assistant_calls);

        if ( resp.tool_calls.empty())
            return normalized;

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

tools::ConfirmMode Repl::tool_mode() const {
    if ( _config.insecure )
        return tools::ConfirmMode::insecure;
    return _config.confirm_tools ? tools::ConfirmMode::confirm
                                 : tools::ConfirmMode::automatic;
}

std::string Repl::handle_command(const std::string& line) {
    std::string cmd, args;
    {
        std::istringstream iss(line);
        iss >> cmd;
        std::getline(iss, args);
        args = common::trim_ws(args);
    }

    if ( cmd == "/help" ) {
        return "commands:\n"
               "  /help                    show this help\n"
               "  /about                   about the app, version, provider/model (alias /info)\n"
               "  /settings                show current settings\n"
               "  /model [name]            show or change the model\n"
               "  /tools <confirm|auto|insecure>   set the tool confirmation mode\n"
               "  /strict <on|off>         also confirm safe read-only commands\n"
               "  /thinking <on|off|low|medium|high|xhigh|max>   thinking level (alias /effort)\n"
               "  /theme <dark|light|warm> switch the colour theme\n"
               "  /history                 list the messages in the current context\n"
               "  /retry                   re-run your last message\n"
               "  /undo                    remove the last exchange from history\n"
               "  /clear                   clear the conversation history\n"
               "  /exit, /quit             leave";
    }

    if ( cmd == "/history" ) {
        std::string s;
        int n = 0;
        for ( const auto& m : _conversation.messages()) {
            if ( m.role == Role::SYSTEM )
                continue;
            std::string who = ( m.role == Role::USER ) ? "you" :
                              ( m.role == Role::ASSISTANT ) ? "ai" : "tool";
            std::string first = m.content.substr(0, m.content.find('\n'));
            if ( first.size() > 70 )
                first = first.substr(0, 70) + "…";
            if ( first.empty() && !m.tool_calls.empty())
                first = "(tool call)";
            s += who + ": " + first + "\n";
            ++n;
        }
        if ( n == 0 )
            return "(no messages yet)";
        s += "\n" + std::to_string(n) + " message(s) in context";
        return s;
    }

    if ( cmd == "/undo" ) {
        std::string removed = _conversation.undo_last();
        if ( removed.empty())
            return "nothing to undo";
        save_conversation();
        return "removed the last exchange";
    }

    if ( cmd == "/retry" ) {
        std::string last = _conversation.undo_last();
        if ( last.empty())
            return "nothing to retry";
        save_conversation();
        return last; // the inline REPL re-submits this as a fresh turn
    }

    if ( cmd == "/info" || cmd == "/about" ) {
        return "agent version 0.1.0\n"
               "A lightweight, local C++ AI assistant for the command line, a\n"
               "provider-agnostic alternative to tools like Kimi Code and Claude Code.\n"
               "It chats, reads/writes files, runs commands and greps — with per-provider\n"
               "history and memory, tool-call safety, and subscription auth for Kimi/Claude.\n"
               "\n"
               "provider:  " + _config.provider + "\n"
               "model:     " + _config.model + "\n"
               "\n"
               "Type /help for commands.";
    }

    if ( cmd == "/settings" ) {
        std::string tools = !_config.tools_enabled ? "off"
                          : ( _config.insecure ? "insecure"
                          : ( _config.confirm_tools ? "confirm" : "auto" ));
        std::string s;
        s += "provider:  " + _config.provider + "\n";
        s += "model:     " + _config.model + "\n";
        s += "tools:     " + tools + ( _config.strict ? " (strict)" : "" ) + "\n";
        s += "thinking:  " + ( _thinking.empty() ? std::string("(provider default)") : _thinking ) + "\n";
        s += "home:      " + _config.home_dir + "\n";
        s += "tokens:    ctx " + std::to_string(_stats.context_tokens.load()) +
             ", session " + std::to_string(_stats.session_total());
        return s;
    }

    if ( cmd == "/model" ) {
        if ( args.empty())
            return "model: " + _config.model;
        _config.model = args;
        if ( _provider )
            _provider->set_model(args);
        return "model set to " + args;
    }

    if ( cmd == "/tools" ) {
        std::string m = common::to_lower(args);
        if ( m == "confirm" ) { _config.confirm_tools = true; _config.insecure = false; }
        else if ( m == "auto" || m == "yes" ) { _config.confirm_tools = false; _config.insecure = false; m = "auto"; }
        else if ( m == "insecure" ) { _config.insecure = true; }
        else return "usage: /tools <confirm|auto|insecure>";
        _registry.set_mode(tool_mode());
        _registry.set_strict(_config.strict);
        return "tool mode: " + m;
    }

    if ( cmd == "/strict" ) {
        std::string m = common::to_lower(args);
        if ( m.empty())
            return std::string("strict: ") + ( _config.strict ? "on" : "off" );
        if ( m == "on" || m == "true" ) _config.strict = true;
        else if ( m == "off" || m == "false" ) _config.strict = false;
        else return "usage: /strict <on|off>";
        _registry.set_strict(_config.strict);
        return std::string("strict: ") + ( _config.strict ? "on" : "off" ) +
               ( _config.strict ? "  (safe read-only commands now also ask)" : "" );
    }

    if ( cmd == "/thinking" || cmd == "/effort" ) {
        if ( args.empty())
            return "thinking: " + ( _thinking.empty() ? std::string("(provider default)") : _thinking );
        _thinking = common::to_lower(args);
        if ( _provider )
            _provider->apply_provider_options(JSON::Object{{ "thinking", _thinking }});
        std::string note = ( _config.provider == "kimi" ) ? "" : "  (only Kimi applies thinking currently)";
        return "thinking: " + _thinking + note;
    }

    if ( cmd == "/clear" ) {
        _conversation.clear();
        std::string system = _config.system_prompt;
        std::string memories = load_memories(_config.home_dir, _config.provider);
        if ( !memories.empty())
            system += memories;
        _conversation.set_system(system);
        save_conversation();
        return "conversation history cleared";
    }

    return "unknown command: " + cmd + " (try /help)";
}

void Repl::run_plain() {

    std::cout << "agent ready. Type /exit or /quit to leave.\n" << std::endl;

    _registry.set_mode(tool_mode());
    _registry.set_strict(_config.strict);
    if ( _config.tools_enabled && tool_mode() != tools::ConfirmMode::insecure ) {
        _registry.set_confirm_callback([](const tools::ConfirmRequest& req) -> tools::Decision {
            if ( !req.danger.empty())
                std::cout << "\n⚠ dangerous command — " << req.danger << std::endl;
            std::cout << "\n" << req.tool << " wants to run:\n" << req.summary << "\n";
            std::cout << "[y] once  [s] session";
            if ( req.can_similar )
                std::cout << "  [a] all `" << req.similar_key << "`";
            std::cout << "  [N] deny\nchoice: " << std::flush;

            std::string answer;
            if ( !std::getline(std::cin, answer))
                return tools::Decision::deny;
            std::string a = common::to_lower(common::trim_ws(answer));
            if ( a == "y" || a == "yes" || a == "once" ) return tools::Decision::once;
            if ( a == "s" || a == "session" )            return tools::Decision::session;
            if ( ( a == "a" || a == "all" ) && req.can_similar ) return tools::Decision::similar;
            return tools::Decision::deny;
        });
    }

    std::string line;
    while ( agent::running.load(std::memory_order_relaxed)) {
        std::cout << "> " << std::flush;
        if ( !std::getline(std::cin, line))
            break;

        if ( common::trim_ws(line).empty())
            continue;

        if ( line == "/exit" || line == "/quit" )
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

    // Keep the transcript clean: in the interactive UI only errors reach the
    // terminal; the full log still goes to the log file (set up in main()).
    logger::loglevel(logger::error);

    InlineRepl inline_repl(
        [this](const std::string& prompt, std::function<void(const std::string&)> stream_cb, std::atomic<bool>* abort_flag) -> std::string {
            try {
                std::string reply = this->process_turn(prompt, stream_cb, abort_flag);
                this->save_conversation();
                return reply;
            } catch ( const std::exception& e ) {
                return std::string("error: ") + e.what();
            }
        },
        _config, _conversation, _stats);

    _registry.set_mode(tool_mode());
    _registry.set_strict(_config.strict);
    if ( _config.tools_enabled && tool_mode() != tools::ConfirmMode::insecure ) {
        _registry.set_confirm_callback([&inline_repl](const tools::ConfirmRequest& req) { return inline_repl.confirm(req); });
    }
    if ( _config.tools_enabled ) {
        _registry.set_activity_callback([&inline_repl](const std::string& a) { inline_repl.set_activity(a); });
    }
    inline_repl.set_command_callback([this](const std::string& cmd) { return handle_command(cmd); });

    inline_repl.run();
}

void Repl::run() {

    if ( isatty(STDIN_FILENO))
        run_tty();
    else
        run_plain();
}

void Repl::run_once(const std::string& prompt) {

    // Non-interactive: no prompt UI. Only insecure/automatic modes can run tools;
    // otherwise confirmation-required tools fail safe (deny).
    _registry.set_mode(tool_mode());
    _registry.set_strict(_config.strict);

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
