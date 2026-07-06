#include "agent/repl.hpp"

#include <iostream>
#include <unistd.h>
#include <cctype>
#include <ctime>
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

// A line stating today's date, appended to the system prompt so the model does not
// have to shell out to `date` (which busybox may lack) to know the current day.
static std::string current_date_line() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    if ( !localtime_r(&t, &tm))
        return "";
    char buf[64];
    if ( std::strftime(buf, sizeof(buf), "%Y-%m-%d (%A)", &tm) == 0 )
        return "";
    return "\n\nThe current date is " + std::string(buf) + ".";
}

Repl::Repl(const Config& config)
    : _config(config), _provider(providers::create(config)) {

    if ( config.tools_enabled )
        _registry.register_defaults();
    else
        logger::info["agent"] << "tools disabled" << std::endl;

    // Re-apply a persisted thinking/effort level (from the previous session) over
    // whatever the provider defaulted to.
    if ( _provider && !_config.thinking.empty())
        _provider->apply_provider_options(JSON::Object{{ "thinking", _config.thinking }});

    // Load any persisted history for THIS provider first, then (re)apply the
    // current system prompt so a provider's identity and freshly-loaded memories
    // always reflect the running config rather than whatever was saved earlier.
    _conversation.load(conversation_path());

    std::string system = config.system_prompt + current_date_line();
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

// Convert the streamed thinking-region markers for plain (non-TTY) output: \x01
// (begin) becomes a "💭 " prefix, \x02 (end) is dropped.
static std::string plain_stream_text(const std::string& s) {
    std::string out;
    for ( char c : s ) {
        if ( c == '\x01' ) out += "\xF0\x9F\x92\xAD ";
        else if ( c != '\x02' ) out += c;
    }
    return out;
}

std::string Repl::process_turn(const std::string& prompt, std::function<void(const std::string&)> stream_cb, std::atomic<bool>* abort_flag) {

    _conversation.add_user(prompt);

    // Whether a dim "thinking" region is currently open in the streamed output.
    // Tracked across tool-loop iterations so the answer after a tool call is not
    // left rendered as dim reasoning.
    bool showing_thinking = false;

    while ( true ) {

        if ( abort_flag && abort_flag->load(std::memory_order_relaxed)) {
            _conversation.undo_last(); // drop the interrupted exchange from history
            return "";
        }

        _provider->prepare_request(_client);

        auto headers = _provider->extra_headers();

        JSON tools = _registry.schema();
        JSON request = _provider->build_request(_conversation, tools);

        // Stream whenever the caller can render live chunks and the provider
        // supports it — including with tools, so reasoning/answer flow live and
        // tool calls are assembled from the stream.
        bool can_stream = stream_cb && _provider->supports_streaming();
        providers::Response resp;

        if ( can_stream ) {
            request["stream"] = true;
            std::string body = request.dump_minified();
            std::string buffer;
            bool done = false;
            _provider->stream_reset();

            _client.post_stream(_provider->endpoint(), _provider->auth_header(), _provider->auth_value(), headers, body,
                [&](const std::string& chunk) {
                    logger::vverbose["http"] << "STREAM chunk\n" << chunk << std::endl;
                    providers::StreamChunk sc = _provider->parse_stream(chunk, buffer, done);
                    if ( _config.thinking_stream && !sc.reasoning.empty()) {
                        // \x01 opens a dim reasoning region (on its own line).
                        if ( !showing_thinking ) { stream_cb("\n\x01"); showing_thinking = true; }
                        stream_cb(sc.reasoning);
                    }
                    if ( !sc.content.empty()) {
                        // \x02 closes the reasoning region before the answer resumes.
                        if ( showing_thinking ) { stream_cb("\n\x02\n\n"); showing_thinking = false; }
                        stream_cb(agent::normalize_text(sc.content));
                    }
                }, abort_flag);

            if ( abort_flag && abort_flag->load(std::memory_order_relaxed)) {
                _conversation.undo_last(); // drop the interrupted exchange from history
                return "";
            }

            resp = _provider->stream_result();
        } else {
            std::string body = request.dump_minified();
            std::string response_str = _client.post(_provider->endpoint(), _provider->auth_header(), _provider->auth_value(), headers, body, abort_flag);

            if ( abort_flag && abort_flag->load(std::memory_order_relaxed)) {
                _conversation.undo_last(); // drop the interrupted exchange from history
                return "";
            }

            resp = _provider->parse_response(JSON::parse(response_str));
        }

        _stats.record(resp.input_tokens, resp.output_tokens);

        if ( !resp.success )
            throws << "provider response error: " << resp.message << std::endl;

        logger::debug["agent"] << "response: content=" << resp.message.size()
                               << "b, thinking=" << resp.thinking.size()
                               << "b, tool_calls=" << resp.tool_calls.size() << std::endl;

        std::string normalized = agent::normalize_text(resp.message);

        std::vector<agent::ToolCall> assistant_calls;
        for ( const auto& tc : resp.tool_calls ) {
            assistant_calls.push_back({ tc.id, tc.name, tc.arguments.dump_minified() });
        }
        // Don't record a degenerate empty assistant turn (no content, no tool
        // calls) — sending it back next request makes some providers return 400.
        if ( !normalized.empty() || !assistant_calls.empty())
            _conversation.add_assistant(normalized, assistant_calls);

        if ( resp.tool_calls.empty()) {
            // In the streaming path the reasoning and answer already went to the
            // live callback. Only the non-streaming fallback prepends the reasoning
            // block here (display only — the saved message keeps just the answer).
            if ( !can_stream && !resp.thinking.empty())
                return "💭 " + agent::normalize_text(resp.thinking) + "\n\n" + normalized;
            return normalized;
        }

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
               "  /settings                open the settings menu (or /settings <key> <value> to set one directly)\n"
               "  /model [name]            show or change the model\n"
               "  /tools <confirm|auto|insecure>   set the tool confirmation mode\n"
               "  /strict <on|off>         also confirm safe read-only commands\n"
               "  /thinking <on|off|low|medium|high|xhigh|max>   thinking level (alias /effort)\n"
               "  /theme <dark|light|warm> switch the colour theme\n"
               "  /stream <on|off>         show the model's reasoning live as it streams\n"
               "  /memories [name]         list this provider's memory files, or view one\n"
               "  /context                 show context usage (system, conversation, limit)\n"
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

    if ( cmd == "/context" ) {
        // The interactive REPL intercepts /context for a visual breakdown; this
        // is the plain-text fallback (non-interactive runs).
        size_t sys = 0, msg = 0;
        for ( const auto& m : _conversation.messages()) {
            size_t t = m.content.size() / 4;
            if ( m.role == Role::SYSTEM ) sys += t; else msg += t;
        }
        std::string s = "context (estimated tokens):\n";
        s += "  system prompt: " + std::to_string(sys) + "\n";
        s += "  conversation:  " + std::to_string(msg) + "\n";
        s += "  total:         " + std::to_string(sys + msg) + "\n";
        s += "  limit:         " + ( _config.context_auto
                 ? ( _config.context_budget() ? "auto (" + std::to_string(_config.context_budget()) + ")" : "auto (unlimited)" )
                 : ( _config.context_limit == 0 ? std::string("unlimited") : std::to_string(_config.context_limit)));
        if ( _stats.context_tokens.load() > 0 )
            s += "\n  last turn:     " + std::to_string(_stats.context_tokens.load());
        return s;
    }

    if ( cmd == "/memories" ) {
        if ( args.empty()) {
            auto files = list_memories(_config.home_dir, _config.provider);
            if ( files.empty())
                return "no memory files for " + _config.provider + "\n(" +
                       _config.home_dir + "/memories/" + _config.provider + ")";
            std::string s = "memories for " + _config.provider + ":\n";
            for ( const auto& f : files )
                s += "  " + f.name + "  (" + std::to_string(f.lines) + " lines)\n";
            s += "\nuse /memories <name> to view one";
            return s;
        }
        std::string content = read_memory(_config.home_dir, _config.provider, args);
        if ( content.empty())
            return "no such memory: " + args;
        return "── " + args + " ──\n" + content;
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
        // With arguments, set a value: /settings <key> <value>. Common settings
        // delegate to their dedicated command so the logic stays in one place.
        if ( !args.empty()) {
            std::istringstream iss(args);
            std::string key, val;
            iss >> key;
            std::getline(iss, val);
            val = common::trim_ws(val);
            key = common::to_lower(key);
            if ( key == "model" ) return handle_command("/model " + val);
            if ( key == "tools" ) return handle_command("/tools " + val);
            if ( key == "strict" ) return handle_command("/strict " + val);
            if ( key == "thinking" || key == "effort" ) return handle_command("/thinking " + val);
            if ( key == "context" || key == "context_limit" ) {
                if ( val.empty())
                    return "usage: /settings context <auto|tokens|0>  (e.g. auto, 64K, 0 = unlimited)";
                if ( common::to_lower(val) == "auto" ) {
                    _config.context_auto = true;
                    size_t b = _config.context_budget();
                    return b ? "context: auto (" + std::to_string(b) + " tokens for " + _config.model + ")"
                             : "context: auto (window unknown for " + _config.model + " → unlimited)";
                }
                _config.context_auto = false;
                _config.context_limit = Config::parse_size_suffixed(val, _config.context_limit);
                return _config.context_limit == 0
                     ? std::string("context: unlimited")
                     : "context: " + std::to_string(_config.context_limit) + " tokens";
            }
            if ( key == "multiline" ) {
                std::string v = common::to_lower(val);
                if ( v == "on" || v == "true" || v == "1" || v == "yes" ) _config.multiline = true;
                else if ( v == "off" || v == "false" || v == "0" || v == "no" ) _config.multiline = false;
                else return "usage: /settings multiline <on|off>";
                return std::string("multiline: ") + ( _config.multiline ? "on" : "off" );
            }
            if ( key == "thinking_stream" || key == "stream" ) {
                std::string v = common::to_lower(val);
                if ( v == "on" || v == "true" || v == "1" || v == "yes" ) _config.thinking_stream = true;
                else if ( v == "off" || v == "false" || v == "0" || v == "no" ) _config.thinking_stream = false;
                else return "usage: /settings thinking_stream <on|off>";
                return std::string("thinking_stream: ") + ( _config.thinking_stream ? "on" : "off" );
            }
            return "unknown setting: " + key + "  (model, tools, strict, thinking, thinking_stream, context, multiline; theme via /theme)";
        }

        std::string tools = !_config.tools_enabled ? "off"
                          : ( _config.insecure ? "insecure"
                          : ( _config.confirm_tools ? "confirm" : "auto" ));
        std::string s;
        s += "provider:  " + _config.provider + "\n";
        s += "model:     " + _config.model + "\n";
        s += "tools:     " + tools + ( _config.strict ? " (strict)" : "" ) + "\n";
        s += "thinking:  " + ( _config.thinking.empty() ? std::string("(provider default)") : _config.thinking ) +
             "  (live stream: " + ( _config.thinking_stream ? "on" : "off" ) + ")\n";
        std::string ctx;
        if ( _config.context_auto ) {
            size_t b = _config.context_budget();
            ctx = b ? "auto (" + std::to_string(b) + " tokens)" : "auto (window unknown → unlimited)";
        } else {
            ctx = _config.context_limit == 0 ? "unlimited" : std::to_string(_config.context_limit) + " tokens";
        }
        s += "context:   " + ctx + "\n";
        s += "multiline: " + std::string( _config.multiline ? "on" : "off" ) + "\n";
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

    if ( cmd == "/stream" ) {
        std::string m = common::to_lower(args);
        if ( m.empty())
            return std::string("stream: ") + ( _config.thinking_stream ? "on" : "off" );
        if ( m == "on" || m == "true" ) _config.thinking_stream = true;
        else if ( m == "off" || m == "false" ) _config.thinking_stream = false;
        else return "usage: /stream <on|off>";
        return std::string("stream: ") + ( _config.thinking_stream ? "on" : "off" );
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
            return "thinking: " + ( _config.thinking.empty() ? std::string("(provider default)") : _config.thinking );
        _config.thinking = common::to_lower(args);
        if ( _provider )
            _provider->apply_provider_options(JSON::Object{{ "thinking", _config.thinking }});
        bool applies = ( _config.provider == "kimi" || _config.provider == "claude" || _config.provider == "anthropic" );
        std::string note = applies ? "" : "  (thinking is applied by Kimi and Claude/Anthropic)";
        return "thinking: " + _config.thinking + note;
    }

    if ( cmd == "/clear" ) {
        _conversation.clear();
        std::string system = _config.system_prompt + current_date_line();
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
                std::cout << plain_stream_text(chunk) << std::flush;
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

    // Persist UI/behaviour settings changed this session (theme, multiline,
    // thinking, context) so they are restored next launch.
    _config.save_settings(_config.home_dir);
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
            std::cout << plain_stream_text(chunk) << std::flush;
        });
        std::cout << std::endl;
        save_conversation();
    } catch ( const std::exception& e ) {
        logger::error << "request failed: " << e.what() << std::endl;
    }
}

} // namespace agent
