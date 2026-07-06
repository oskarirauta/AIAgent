#include "agent/repl.hpp"

#include <iostream>
#include <unistd.h>
#include <cctype>
#include <ctime>
#include <algorithm>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "json.hpp"
#include "logger.hpp"
#include "common.hpp"
#include "throws.hpp"
#include "agent/repl_inline.hpp"
#include "agent/signal_handler.hpp"
#include "agent/memory.hpp"
#include "agent/text_utils.hpp"
#include "agent/tools/advisor.hpp"
#include "agent/tools/workflow_tool.hpp"
#include "agent/tools/web_search.hpp"
#include "agent/tools/mcp_tool.hpp"

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
    _conversation.set_system(base_system_prompt());

    // Expose the advisor tool if it was left enabled and the provider supports it.
    sync_advisor_tool();
    sync_workflow_tool();
    sync_web_search_tool();
    connect_mcp();

    // Snapshot files before write_file overwrites them, for /changes + revert.
    _registry.set_pre_run_callback([this](const std::string& n, const JSON& a) {
        record_file_change(n, a);
    });
}

void Repl::record_file_change(const std::string& tool, const JSON& args) {
    if ( tool != "write_file" || args != JSON::TYPE::OBJECT || !args.contains("path"))
        return;
    std::string p = common::trim_ws(args["path"].to_string());
    if ( p.empty())
        return;
    std::string abs;
    try { abs = std::filesystem::absolute(p).string(); } catch ( ... ) { abs = p; }
    if ( _changes.count(abs))
        return; // keep the earliest (session-start) snapshot

    FileChange fc;
    std::error_code ec;
    if ( std::filesystem::is_regular_file(abs, ec)) {
        fc.existed = true;
        auto sz = std::filesystem::file_size(abs, ec);
        if ( !ec && sz <= 5 * 1024 * 1024 ) {
            std::ifstream ifd(abs, std::ios::binary);
            std::stringstream ss; ss << ifd.rdbuf();
            fc.original = ss.str();
            fc.tracked = true;
        } else {
            fc.tracked = false; // too large to snapshot for revert
        }
    } else {
        fc.existed = false; // brand-new file
        fc.tracked = true;
    }
    _changes[abs] = fc;
}

// Read a file's current content, or nullopt if it doesn't exist.
static std::optional<std::string> read_current(const std::string& path) {
    std::error_code ec;
    if ( !std::filesystem::is_regular_file(path, ec))
        return std::nullopt;
    std::ifstream ifd(path, std::ios::binary);
    if ( !ifd.is_open())
        return std::nullopt;
    std::stringstream ss; ss << ifd.rdbuf();
    return ss.str();
}

static size_t count_lines(const std::string& s) {
    if ( s.empty()) return 0;
    size_t n = static_cast<size_t>(std::count(s.begin(), s.end(), '\n'));
    if ( s.back() != '\n' ) ++n;
    return n;
}

// A compact block diff: trim the common leading/trailing lines and show the rest
// as -old / +new with a little context.
static std::string block_diff(const std::string& oldc, const std::string& newc) {
    auto split = [](const std::string& s) {
        std::vector<std::string> v; std::string line; std::istringstream is(s);
        while ( std::getline(is, line)) v.push_back(line);
        return v;
    };
    std::vector<std::string> o = split(oldc), n = split(newc);
    size_t p = 0;
    while ( p < o.size() && p < n.size() && o[p] == n[p] ) ++p;
    size_t s = 0;
    while ( s < o.size() - p && s < n.size() - p && o[o.size() - 1 - s] == n[n.size() - 1 - s] ) ++s;

    std::string out = "--- original\n+++ current\n";
    size_t ctx_start = p > 2 ? p - 2 : 0;
    for ( size_t i = ctx_start; i < p; ++i ) out += "  " + o[i] + "\n";
    size_t shown = 0;
    for ( size_t i = p; i < o.size() - s && shown < 200; ++i, ++shown ) out += "- " + o[i] + "\n";
    shown = 0;
    for ( size_t i = p; i < n.size() - s && shown < 200; ++i, ++shown ) out += "+ " + n[i] + "\n";
    size_t ctx_end = std::min(o.size(), ( o.size() - s ) + 2 );
    for ( size_t i = o.size() - s; i < ctx_end; ++i ) out += "  " + o[i] + "\n";
    return out;
}

std::string Repl::changes_command(const std::string& args) {
    std::istringstream iss(args);
    std::string sub, target;
    iss >> sub; { std::string rest; std::getline(iss, rest); target = common::trim_ws(rest); }
    sub = common::to_lower(sub);

    if ( sub == "revert" ) {
        if ( target.empty())
            return "usage: /changes revert <path|all>";
        auto revert_one = [this](const std::string& abs, FileChange& fc) -> std::string {
            if ( !fc.tracked )
                return "skip " + abs + " (too large to snapshot)";
            std::error_code ec;
            if ( fc.existed ) {
                std::ofstream ofd(abs, std::ios::binary | std::ios::trunc);
                ofd << fc.original;
                return "reverted " + abs;
            }
            std::filesystem::remove(abs, ec);
            return "removed " + abs + " (did not exist at session start)";
        };
        if ( common::to_lower(target) == "all" ) {
            if ( _changes.empty()) return "no changes to revert";
            std::string s;
            for ( auto& [abs, fc] : _changes ) s += revert_one(abs, fc) + "\n";
            _changes.clear();
            return s;
        }
        std::string abs;
        try { abs = std::filesystem::absolute(target).string(); } catch ( ... ) { abs = target; }
        auto it = _changes.find(abs);
        if ( it == _changes.end())
            return "no tracked change for " + target;
        std::string r = revert_one(abs, it->second);
        _changes.erase(it);
        return r;
    }

    if ( sub == "diff" ) {
        if ( target.empty())
            return "usage: /changes diff <path>";
        std::string abs;
        try { abs = std::filesystem::absolute(target).string(); } catch ( ... ) { abs = target; }
        auto it = _changes.find(abs);
        if ( it == _changes.end())
            return "no tracked change for " + target;
        if ( !it->second.tracked )
            return target + " was too large to snapshot; no diff available";
        auto cur = read_current(abs);
        if ( !it->second.existed )
            return target + " was created this session (" +
                   std::to_string(count_lines(cur.value_or(""))) + " lines)";
        return block_diff(it->second.original, cur.value_or(""));
    }

    if ( _changes.empty())
        return "no files changed this session";

    std::string s = "files changed this session:\n";
    for ( const auto& [abs, fc] : _changes ) {
        auto cur = read_current(abs);
        std::string status;
        if ( !fc.existed && cur ) status = "created";
        else if ( fc.existed && !cur ) status = "deleted";
        else if ( fc.tracked && cur && *cur == fc.original ) status = "reverted/unchanged";
        else status = "modified";

        s += "\n  " + status + "  " + abs;
        if ( fc.tracked && fc.existed && cur && status == "modified" ) {
            long d = static_cast<long>(count_lines(*cur)) - static_cast<long>(count_lines(fc.original));
            s += std::string("  (") + ( d >= 0 ? "+" : "" ) + std::to_string(d) + " lines)";
        }
    }
    s += "\n\n/changes diff <path> · /changes revert <path|all>";
    return s;
}

std::string Repl::export_transcript(const std::string& path) {
    const auto& msgs = _conversation.messages();
    std::string md = "# Conversation export\n\n";
    std::string date = current_date_line();
    if ( date.size() > 2 ) date = " — " + date.substr(2);
    md += "*" + _config.provider + " · " + _config.model + date + "*\n";

    int n = 0;
    for ( const auto& m : msgs ) {
        std::string heading;
        switch ( m.role ) {
            case Role::SYSTEM:    heading = "## System"; break;
            case Role::USER:      heading = "## You"; break;
            case Role::ASSISTANT: heading = "## Assistant"; break;
            case Role::TOOL:      heading = "### Tool result" +
                                   ( m.name ? ( " (" + *m.name + ")" ) : std::string()); break;
        }
        md += "\n" + heading + "\n\n";
        if ( !m.content.empty())
            md += m.content + "\n";
        for ( const auto& tc : m.tool_calls )
            md += "\n- calls `" + tc.name + "(" + tc.arguments + ")`\n";
        ++n;
    }

    std::ofstream ofd(path, std::ios::out | std::ios::trunc);
    if ( !ofd.is_open())
        return "error: cannot write " + path;
    ofd << md;
    ofd.flush();
    return "exported " + std::to_string(n) + " message(s) to " + path +
           " (" + std::to_string(md.size()) + " bytes)";
}

void Repl::connect_mcp() {
    if ( !_config.tools_enabled )
        return; // no point spawning servers if their tools can't be called

    int n = 0;
    if ( !_config.mcp_config.empty()) {
        n = _mcp.load_config(_config.mcp_config);
    } else {
        n += _mcp.load_config(_config.home_dir + "/mcp.json");
        n += _mcp.load_config(".mcp.json"); // project-local
    }
    if ( n == 0 )
        return;

    _mcp.connect_all();
    register_mcp_tools();
}

void Repl::register_mcp_tools() {
    // Drop previously-registered MCP tools, then register the current set (called
    // after connect and after /mcp refresh).
    for ( const auto& name : _mcp_tool_names )
        _registry.remove(name);
    _mcp_tool_names.clear();

    auto sanitise = [](const std::string& s) {
        std::string o;
        for ( char c : s )
            o += ( std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ) ? c : '_';
        return o;
    };

    for ( const auto& t : _mcp.tools()) {
        _registry.add(std::make_unique<tools::McpTool>(
            t.registered, t.description, t.input_schema, t.server, t.tool,
            [this](const std::string& s, const std::string& tool, const JSON& a) {
                return _mcp.call_tool(s, tool, a);
            }));
        _mcp_tool_names.push_back(t.registered);
    }

    // One read_resource tool per server that exposes resources; its description
    // enumerates the available URIs so the model knows what it can read.
    std::map<std::string, std::vector<mcp::ResourceDef>> by_server;
    for ( const auto& r : _mcp.resources())
        by_server[r.server].push_back(r);
    for ( const auto& [server, rs] : by_server ) {
        std::string desc = "Read a resource from MCP server '" + server + "'. Available resources:\n";
        for ( const auto& r : rs )
            desc += "  " + r.uri + ( r.description.empty() ? "" : "  — " + r.description ) + "\n";
        JSON schema = JSON::Object{
            { "type", "object" },
            { "properties", JSON::Object{ { "uri", JSON::Object{
                { "type", "string" }, { "description", "the resource URI to read" } } } } },
            { "required", JSON::Array{ "uri" } }
        };
        std::string reg = "mcp__" + sanitise(server) + "__read_resource";
        _registry.add(std::make_unique<tools::McpTool>(
            reg, desc, schema, server, "read_resource",
            [this](const std::string& s, const std::string&, const JSON& a) {
                std::string uri = ( a == JSON::TYPE::OBJECT && a.contains("uri")) ? a["uri"].to_string() : "";
                return _mcp.read_resource(s, uri);
            }));
        _mcp_tool_names.push_back(reg);
    }
}

std::string Repl::mcp_command(const std::string& args) {
    if ( !_mcp.any_configured())
        return "no MCP servers configured\n"
               "add them to " + _config.home_dir + "/mcp.json or ./.mcp.json, e.g.\n"
               "  {\"mcpServers\": {\"filesystem\": {\"command\": \"npx\", "
               "\"args\": [\"-y\", \"@modelcontextprotocol/server-filesystem\", \".\"]}}}\n"
               "or an HTTP server: {\"mcpServers\": {\"remote\": {\"url\": \"https://host/mcp\"}}}";

    std::istringstream iss(args);
    std::string sub;
    iss >> sub;
    sub = common::to_lower(sub);

    if ( sub == "refresh" ) {
        int n = _mcp.refresh();
        register_mcp_tools();
        return "refreshed MCP servers — " + std::to_string(n) + " tool(s) available";
    }

    if ( sub == "prompt" ) {
        std::string server, name;
        iss >> server >> name;
        if ( server.empty() || name.empty())
            return "usage: /mcp prompt <server> <name> [key=value ...]";
        std::map<std::string, std::string> pargs;
        std::string kv;
        while ( iss >> kv ) {
            size_t eq = kv.find('=');
            if ( eq != std::string::npos )
                pargs[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
        std::string text = _mcp.get_prompt(server, name, pargs);
        if ( text.rfind("error:", 0) == 0 )
            return text;
        _conversation.add_user(text);
        save_conversation();
        return "loaded prompt '" + name + "' from " + server +
               " into the context (send a message to continue):\n\n" + text;
    }

    auto st = _mcp.status();
    std::string s = "MCP servers:\n";
    for ( const auto& si : st ) {
        s += std::string("\n  ") + ( si.connected ? "✓" : "✗" ) + " " + si.name +
             "  [" + si.transport + "]";
        if ( si.connected ) {
            s += "\n      tools: ";
            if ( si.tool_names.empty()) s += "(none)";
            else for ( size_t i = 0; i < si.tool_names.size(); ++i )
                s += ( i ? ", " : "" ) + si.tool_names[i];
            if ( !si.resource_uris.empty()) {
                s += "\n      resources: " + std::to_string(si.resource_uris.size()) +
                     " (read via mcp__" + si.name + "__read_resource)";
            }
            if ( !si.prompt_names.empty()) {
                s += "\n      prompts: ";
                for ( size_t i = 0; i < si.prompt_names.size(); ++i )
                    s += ( i ? ", " : "" ) + si.prompt_names[i];
            }
        } else {
            s += "  — " + ( si.error.empty() ? std::string("not connected") : si.error );
        }
    }
    s += "\n\nmodel calls tools as mcp__<server>__<tool>"
         "\n/mcp refresh — re-list · /mcp prompt <server> <name> [k=v] — load a prompt";
    return s;
}

void Repl::sync_web_search_tool() {
    bool want = _config.tools_enabled && _config.web_search;
    if ( want && !_registry.has("web_search"))
        _registry.add(std::make_unique<tools::WebSearch>(_config.web_search_url));
    else if ( !want && _registry.has("web_search"))
        _registry.remove("web_search");
}

void Repl::sync_workflow_tool() {
    // The run_workflow tool is available when tools are on and the provider can
    // drive background sub-agents (claude). Kept in sync across /provider switches.
    bool want = _config.tools_enabled && provider_supports("workflows");
    if ( want && !_registry.has("run_workflow")) {
        _registry.add(std::make_unique<tools::WorkflowTool>(
            [this](const std::string& name, const std::vector<std::string>& steps) {
                Config cfg = _config; // snapshot: sub-agents must not read live config
                int id = _workflows.launch(name, steps,
                    [cfg](const std::string& task, std::atomic<bool>* abort) {
                        return run_workflow_step(cfg, task, abort);
                    });
                return "started workflow #" + std::to_string(id) + " (" +
                       std::to_string(steps.size()) + " step(s)) in the background; "
                       "watch it with /workflows. Results will come back on your next turn.";
            }));
    } else if ( !want && _registry.has("run_workflow")) {
        _registry.remove("run_workflow");
    }
}

std::string Repl::workflows_command(const std::string& args) {
    std::string a = common::trim_ws(args);
    auto runs = _workflows.snapshot();

    auto step_glyph = [](const std::string& st) -> std::string {
        if ( st == "done" ) return "✓";
        if ( st == "error" ) return "✗";
        if ( st == "running" ) return "▸";
        if ( st == "cancelled" ) return "∅";
        return "·"; // pending
    };

    if ( !a.empty()) {
        // Detail view for one run.
        int want = 0;
        try { want = std::stoi(a); } catch ( ... ) { return "usage: /workflows [id]"; }
        for ( const auto& r : runs ) {
            if ( r.id != want ) continue;
            std::string s = "workflow #" + std::to_string(r.id) + "  " + r.name +
                            "  [" + r.status + "]\n";
            for ( size_t i = 0; i < r.steps.size(); ++i ) {
                const auto& st = r.steps[i];
                s += "\n" + step_glyph(st.status) + " step " + std::to_string(i + 1) +
                     ": " + st.task + "\n";
                if ( !st.result.empty())
                    s += "  " + st.result + "\n";
            }
            return s;
        }
        return "no workflow #" + a;
    }

    if ( runs.empty())
        return "no workflows yet — the model starts one with the run_workflow tool";

    std::string s = "workflows:\n";
    for ( const auto& r : runs ) {
        int done = 0;
        for ( const auto& st : r.steps )
            if ( st.status == "done" || st.status == "error" ) ++done;
        s += "\n  #" + std::to_string(r.id) + "  " + r.name +
             "  [" + r.status + "]  " + std::to_string(done) + "/" +
             std::to_string(r.steps.size()) + " steps";
    }
    s += "\n\nuse /workflows <id> for step details";
    return s;
}

void Repl::deliver_workflow_results() {
    // Called on the turn thread before a new turn: fold any finished runs into the
    // conversation so the model can build on them. Only this thread mutates
    // _conversation, and the workflow threads for these runs have already ended.
    auto done = _workflows.take_undelivered();
    for ( const auto& r : done ) {
        std::string note = "Workflow #" + std::to_string(r.id) + " (" + r.name +
                           ") finished with status: " + r.status + ".\n";
        for ( size_t i = 0; i < r.steps.size(); ++i ) {
            note += "\nStep " + std::to_string(i + 1) + " [" + r.steps[i].status + "]: " +
                    r.steps[i].task + "\nResult: " + r.steps[i].result + "\n";
        }
        _conversation.add_user(note);
    }
}

void Repl::sync_advisor_tool() {
    // The consult_advisor tool is available only when advisor mode is on, tools
    // are enabled, and the provider can reach an advisor model (claude).
    bool want = _config.advisor && _config.tools_enabled && provider_supports("advisor");
    if ( want && !_registry.has("consult_advisor"))
        _registry.add(std::make_unique<tools::AdvisorTool>(
            [this](const std::string& q) { return ask_advisor(q); }));
    else if ( !want && _registry.has("consult_advisor"))
        _registry.remove("consult_advisor");
}

std::string Repl::ask_advisor(const std::string& question) {
    if ( !_provider )
        return "advisor unavailable: no provider";

    // A self-contained one-shot consult (no tools, no streaming), sent to the
    // advisor model by temporarily swapping the model on the active provider so
    // it reuses the same authenticated session.
    Conversation adv;
    adv.set_system(
        "You are a senior software engineer acting as an advisor to another AI coding "
        "assistant that is stuck or wants a second opinion on a hard problem. Give "
        "focused, correct, actionable guidance: the right approach, likely pitfalls, and "
        "concrete next steps. You cannot run tools or see the repository, so reason from "
        "what the question describes. Be concise and specific.");
    adv.add_user(question);

    std::string previous_model = _provider->model();
    _provider->set_model(_config.advisor_model);
    std::string advice;
    try {
        _provider->prepare_request(_client);
        JSON req = _provider->build_request(adv, JSON::Array{});
        std::string body = req.dump_minified();
        std::string resp = _client.post(_provider->endpoint(), _provider->auth_header(),
                                        _provider->auth_value(), _provider->extra_headers(),
                                        body, &agent::turn_abort);
        if ( agent::turn_abort.load(std::memory_order_relaxed) || resp.empty()) {
            advice = "advisor consult cancelled";
        } else {
            auto parsed = _provider->parse_response(JSON::parse(resp));
            advice = agent::normalize_text(parsed.message);
            if ( advice.empty())
                advice = "the advisor returned no advice";
        }
    } catch ( const std::exception& e ) {
        advice = std::string("advisor error: ") + e.what();
    }
    _provider->set_model(previous_model);
    return "Advice from " + _config.advisor_model + ":\n\n" + advice;
}

std::string Repl::base_system_prompt() const {
    std::string system = _config.system_prompt + current_date_line();
    std::string memories = load_memories(_config.home_dir, _config.provider);
    if ( !memories.empty())
        system += memories;
    // Project-local instructions from the working directory (AGENTS.md etc.),
    // read fresh so switching projects / dirs always reflects the current one.
    std::string project;
    try {
        project = load_project_instructions(std::filesystem::current_path().string());
    } catch ( ... ) {
        project = "";
    }
    if ( !project.empty())
        system += project;
    return system;
}

std::string Repl::compact_history() {
    const auto& msgs = _conversation.messages();
    size_t count = 0;
    for ( const auto& m : msgs )
        if ( m.role != Role::SYSTEM ) ++count;
    if ( count < 4 )
        return "nothing to compact (conversation is already short)";

    // Render the transcript for the summariser.
    std::string transcript;
    for ( const auto& m : msgs ) {
        if ( m.role == Role::SYSTEM ) continue;
        const char* who = m.role == Role::USER ? "User"
                        : m.role == Role::ASSISTANT ? "Assistant" : "Tool";
        if ( !m.content.empty())
            transcript += std::string(who) + ": " + m.content + "\n";
        for ( const auto& tc : m.tool_calls )
            transcript += "Assistant (tool " + tc.name + "): " + tc.arguments + "\n";
    }

    // One-shot summarisation (no tools, no streaming).
    Conversation summ;
    summ.set_system("You compress a coding-assistant conversation into a concise briefing "
        "the assistant can continue from. Preserve the user's goals, decisions made, key file "
        "paths and code facts, and any unfinished tasks. Be faithful — do not invent details. "
        "Output only the summary.");
    summ.add_user("Summarise this conversation so it can replace the full history:\n\n" + transcript);

    _provider->prepare_request(_client);
    JSON req = _provider->build_request(summ, JSON::Array{});
    std::string body = req.dump_minified();
    std::string resp_str = _client.post(_provider->endpoint(), _provider->auth_header(),
                                        _provider->auth_value(), _provider->extra_headers(), body,
                                        &agent::turn_abort);
    if ( agent::turn_abort.load(std::memory_order_relaxed) || resp_str.empty())
        return "compact cancelled";
    auto resp = _provider->parse_response(JSON::parse(resp_str));
    std::string summary = agent::normalize_text(resp.message);
    if ( !resp.success || summary.empty())
        throws << "summarisation returned no content" << std::endl;

    // Replace history with [system, user(summary), assistant(ack)] — a valid
    // alternating start for every provider on the next turn.
    _conversation.clear();
    _conversation.set_system(base_system_prompt());
    _conversation.add_user("Summary of our conversation so far — use this as context:\n\n" + summary);
    _conversation.add_assistant("Understood — I'll continue with that context in mind.");
    save_conversation();

    return "compacted " + std::to_string(count) + " messages into a summary";
}

std::string Repl::switch_provider(const std::string& name) {
    static const std::vector<std::string> supported =
        { "openai", "ollama", "anthropic", "moonshot", "kimi", "claude" };
    if ( std::find(supported.begin(), supported.end(), name) == supported.end())
        return "unknown provider: " + name +
               "  (openai, ollama, anthropic, moonshot, kimi, claude)";
    if ( name == _config.provider )
        return "already using " + name;

    // Build the target config, mirroring main()'s resolution: reset the endpoint
    // so the new provider picks its own default, resolve its remembered/default
    // model, and swap in a provider-appropriate identity unless the user set a
    // custom system prompt.
    Config nc = _config;
    nc.provider = name;
    nc.provider_explicit = true;
    nc.api_url = Config().api_url; // provider constructors override this to their own default
    if ( name == "kimi" )
        nc.api_url = "https://api.kimi.com/coding/v1";

    Config::LastUsed last = Config::load_last_used(_config.home_dir);
    std::string remembered = last.model_for(name);
    nc.model = !remembered.empty() ? remembered : Config::default_model_for(name);
    nc.model_explicit = true;

    if ( _config.system_prompt == Config::default_system_prompt_for(_config.provider))
        nc.system_prompt = Config::default_system_prompt_for(name);

    // Construct and, for subscription providers, make sure we can authenticate
    // without an interactive login (the REPL is in raw mode). If not, refuse the
    // switch rather than blocking on a URL/code prompt.
    std::unique_ptr<providers::Provider> np;
    try {
        np = providers::create(nc);
    } catch ( const std::exception& e ) {
        return std::string("could not switch to ") + name + ": " + e.what();
    }
    if ( np && !nc.thinking.empty())
        np->apply_provider_options(JSON::Object{{ "thinking", nc.thinking }});

    if ( name == "kimi" || name == "claude" ) {
        if ( !np->ready_noninteractive(_client))
            return "not logged in to " + name +
                   " — relaunch with `-p " + name + "` to log in first, then switch back.";
    }

    // Commit the switch. _config / _conversation are shared by reference with the
    // running InlineRepl, so the status line and settings reflect the change at
    // once. The conversation is carried over: keep the dialogue, but refresh the
    // system message to the new provider's identity + memories.
    _config = nc;
    _provider = std::move(np);
    _conversation.set_system(base_system_prompt());
    sync_advisor_tool();  // advisor tool follows the provider (claude-only)
    sync_workflow_tool(); // workflow tool follows the provider (claude-only)
    save_conversation();
    Config::save_last_used(_config.home_dir, _config.provider, _config.model);

    return "switched to " + name + " (" + _config.model + ") — the conversation continues here";
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

    // Fold any finished background workflows into the context first, so the model
    // sees their results ahead of the new prompt.
    deliver_workflow_results();

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
               "  /provider [name]         switch provider mid-session (carries the conversation over)\n"
               "  /model [name]            show or change the model\n"
               "  /btw <note>              add a note to the context without asking for a reply (alias /note)\n"
               "  /advisor <on|off|model N>   (claude) let the model consult a stronger advisor model\n"
               "  /workflows [id]          (claude) view background workflow runs the model started\n"
               "  /mcp [refresh|prompt <server> <name> [k=v]]   MCP servers, tools, resources, prompts\n"
               "  /tools <confirm|auto|insecure>   set the tool confirmation mode\n"
               "  /strict <on|off>         also confirm safe read-only commands\n"
               "  /thinking <on|off|low|medium|high|xhigh|max>   thinking level (alias /effort)\n"
               "  /theme <dark|light|warm> switch the colour theme\n"
               "  /stream <off|on|collapse> live reasoning; collapse hides it once the answer is done\n"
               "  /memories [name]         list this provider's memory files, or view one\n"
               "  /context                 show context usage (system, conversation, limit)\n"
               "  /cost [budget <usd>|tokens <n>]   session token usage + estimated cost / budget\n"
               "  /history                 list the messages in the current context\n"
               "  /retry                   re-run your last message\n"
               "  /undo                    remove the last exchange from history\n"
               "  /changes [diff|revert <path|all>]   files the agent changed this session\n"
               "  /export [file]           write the conversation to a Markdown file\n"
               "  /clear (/reset)          clear the conversation history\n"
               "  /compact                 summarise the history to free up context\n"
               "  /settings auto_compact <on|off>   auto-compact when the context nears its budget\n"
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

    if ( cmd == "/cost" ) {
        if ( !args.empty()) {
            std::istringstream iss(args);
            std::string sub, val;
            iss >> sub >> val;
            sub = common::to_lower(sub);
            if ( sub == "budget" ) {
                if ( val.empty()) return "usage: /cost budget <usd>  (0 = off)";
                try { _config.budget_usd = std::stod(val); }
                catch ( ... ) { return "usage: /cost budget <usd>"; }
                return _config.budget_usd > 0
                     ? "cost budget: $" + val
                     : std::string("cost budget: off");
            }
            if ( sub == "tokens" ) {
                if ( val.empty()) return "usage: /cost tokens <n>  (0 = off)";
                _config.budget_tokens = Config::parse_size_suffixed(val, _config.budget_tokens);
                return _config.budget_tokens > 0
                     ? "token budget: " + std::to_string(_config.budget_tokens)
                     : std::string("token budget: off");
            }
            return "usage: /cost [budget <usd> | tokens <n>]";
        }

        long in = _stats.session_input.load(std::memory_order_relaxed);
        long out = _stats.session_output.load(std::memory_order_relaxed);
        long total = in + out;
        std::string s = "session usage (" + _config.provider + " · " + _config.model + "):\n";
        s += "  input:   " + std::to_string(in) + " tokens\n";
        s += "  output:  " + std::to_string(out) + " tokens\n";
        s += "  total:   " + std::to_string(total) + " tokens\n";

        double cost = _config.session_cost(in, out);
        if ( cost < 0 ) {
            s += "  cost:    (no price configured for this model — usage only)\n";
            s += "           set one with `price." + _config.model + ": <in>/<out>` (USD per Mtok)";
        } else {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "$%.4f", cost);
            s += "  cost:    " + std::string(buf);
            if ( _config.budget_usd > 0 ) {
                int pct = static_cast<int>(cost / _config.budget_usd * 100.0);
                char bb[64];
                std::snprintf(bb, sizeof(bb), "  of $%.2f budget (%d%%)", _config.budget_usd, pct);
                s += std::string(bb);
            }
        }
        if ( _config.budget_tokens > 0 ) {
            int pct = static_cast<int>(static_cast<double>(total) / _config.budget_tokens * 100.0);
            s += "\n  tokens:  " + std::to_string(total) + " / " +
                 std::to_string(_config.budget_tokens) + " (" + std::to_string(pct) + "%)";
        }
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

    if ( cmd == "/changes" ) {
        return changes_command(args);
    }

    if ( cmd == "/export" ) {
        std::string path = common::trim_ws(args);
        if ( path.empty()) {
            std::time_t t = std::time(nullptr);
            std::tm tm{};
            char buf[32] = "export";
            if ( localtime_r(&t, &tm))
                std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
            path = "agent-export-" + std::string(buf) + ".md";
        }
        return export_transcript(path);
    }

    if ( cmd == "/btw" || cmd == "/note" ) {
        if ( args.empty())
            return "usage: /btw <note>  — add a note to the context without asking for a reply";
        _conversation.add_user(args);
        save_conversation();
        return "noted — added to the context (no reply); the model will see it on your next message";
    }

    if ( cmd == "/provider" ) {
        if ( args.empty())
            return "provider: " + _config.provider +
                   "\nusage: /provider <openai|ollama|anthropic|moonshot|kimi|claude>";
        return switch_provider(common::to_lower(common::trim_ws(args)));
    }

    if ( cmd == "/mcp" ) {
        return mcp_command(args);
    }

    if ( cmd == "/workflows" || cmd == "/workflow" ) {
        if ( !provider_supports("workflows"))
            return "workflows are only available with the claude provider";
        return workflows_command(args);
    }

    if ( cmd == "/advisor" ) {
        if ( !provider_supports("advisor"))
            return "advisor is only available with the claude provider "
                   "(the model consults a stronger Claude model for a second opinion)";
        std::istringstream iss(args);
        std::string sub;
        iss >> sub;
        sub = common::to_lower(sub);
        std::string rest;
        std::getline(iss, rest);
        rest = common::trim_ws(rest);

        auto status = [this]() -> std::string {
            std::string s = std::string("advisor: ") + ( _config.advisor ? "on" : "off" ) +
                            "  (model: " + _config.advisor_model + ")";
            if ( _config.advisor && !_config.tools_enabled )
                s += "\nnote: tools are disabled, so the model cannot reach the advisor";
            return s;
        };

        if ( sub.empty())
            return status();
        if ( sub == "on" || sub == "true" || sub == "yes" ) _config.advisor = true;
        else if ( sub == "off" || sub == "false" || sub == "no" ) _config.advisor = false;
        else if ( sub == "model" ) {
            if ( rest.empty())
                return "usage: /advisor model <name>  (e.g. claude-opus-4-8)";
            _config.advisor_model = rest;
        }
        else return "usage: /advisor <on|off|model <name>>";

        sync_advisor_tool();
        save_conversation();
        return status();
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
               "model:     " + _config.model + "\n" +
               ( []() {
                   std::string f;
                   try { f = agent::project_instructions_file(std::filesystem::current_path().string()); }
                   catch ( ... ) {}
                   return f.empty() ? std::string() : "project:   " + f + " loaded\n";
               }()) +
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
                if ( v == "on" || v == "true" || v == "1" || v == "yes" ) { _config.thinking_stream = true; _config.thinking_collapse = false; }
                else if ( v == "off" || v == "false" || v == "0" || v == "no" ) { _config.thinking_stream = false; _config.thinking_collapse = false; }
                else if ( v == "collapse" || v == "hide" ) { _config.thinking_stream = true; _config.thinking_collapse = true; }
                else return "usage: /settings thinking_stream <off|on|collapse>";
                return std::string("stream: ") + ( !_config.thinking_stream ? "off" : ( _config.thinking_collapse ? "collapse" : "on" ));
            }
            if ( key == "paste_preview" || key == "preview" ) {
                std::string v = common::to_lower(val);
                if ( v == "off" || v == "all" || v == "none" ) _config.paste_preview = 0;
                else _config.paste_preview = Config::parse_size_suffixed(val, _config.paste_preview);
                return _config.paste_preview == 0
                     ? std::string("paste preview: all lines")
                     : "paste preview: first " + std::to_string(_config.paste_preview) + " lines";
            }
            if ( key == "auto_compact" || key == "autocompact" ) {
                std::string v = common::to_lower(val);
                if ( v == "on" || v == "true" || v == "1" || v == "yes" ) _config.auto_compact = true;
                else if ( v == "off" || v == "false" || v == "0" || v == "no" ) _config.auto_compact = false;
                else return "usage: /settings auto_compact <on|off>";
                if ( _config.auto_compact && _config.context_budget() == 0 )
                    return "auto-compact: on  (inactive: set a context limit — /settings context auto)";
                return std::string("auto-compact: ") + ( _config.auto_compact ? "on" : "off" ) +
                       ( _config.auto_compact ? "  (at " + std::to_string(_config.auto_compact_pct) + "% of the context budget)" : "" );
            }
            if ( key == "web_search" || key == "websearch" ) {
                std::string v = common::to_lower(val);
                if ( v == "on" || v == "true" || v == "1" || v == "yes" ) _config.web_search = true;
                else if ( v == "off" || v == "false" || v == "0" || v == "no" ) _config.web_search = false;
                else return "usage: /settings web_search <on|off>";
                sync_web_search_tool();
                return std::string("web_search: ") + ( _config.web_search ? "on" : "off" );
            }
            if ( key == "advisor" ) return handle_command("/advisor " + val);
            if ( key == "advisor_model" ) return handle_command("/advisor model " + val);
            if ( key == "auto_compact_pct" ) {
                size_t p = Config::parse_size_suffixed(val, _config.auto_compact_pct);
                if ( p < 10 || p > 100 )
                    return "usage: /settings auto_compact_pct <10-100>";
                _config.auto_compact_pct = p;
                return "auto-compact threshold: " + std::to_string(_config.auto_compact_pct) + "% of the context budget";
            }
            return "unknown setting: " + key + "  (model, tools, strict, thinking, thinking_stream, paste_preview, context, auto_compact, advisor, web_search, multiline; theme via /theme)";
        }

        std::string tools = !_config.tools_enabled ? "off"
                          : ( _config.insecure ? "insecure"
                          : ( _config.confirm_tools ? "confirm" : "auto" ));
        std::string s;
        s += "provider:  " + _config.provider + "\n";
        s += "model:     " + _config.model + "\n";
        s += "tools:     " + tools + ( _config.strict ? " (strict)" : "" ) + "\n";
        s += "thinking:  " + ( _config.thinking.empty() ? std::string("(provider default)") : _config.thinking ) +
             "  (stream: " + ( !_config.thinking_stream ? "off" : ( _config.thinking_collapse ? "collapse" : "on" )) + ")\n";
        std::string ctx;
        if ( _config.context_auto ) {
            size_t b = _config.context_budget();
            ctx = b ? "auto (" + std::to_string(b) + " tokens)" : "auto (window unknown → unlimited)";
        } else {
            ctx = _config.context_limit == 0 ? "unlimited" : std::to_string(_config.context_limit) + " tokens";
        }
        s += "context:   " + ctx + "\n";
        s += "auto_compact: " + std::string( _config.auto_compact ? "on" : "off" ) +
             ( _config.auto_compact ? "  (at " + std::to_string(_config.auto_compact_pct) + "%)" : "" ) + "\n";
        if ( provider_supports("advisor"))
            s += "advisor:   " + std::string( _config.advisor ? "on" : "off" ) +
                 "  (model: " + _config.advisor_model + ")\n";
        s += "multiline: " + std::string( _config.multiline ? "on" : "off" ) + "\n";
        s += "web_search: " + std::string( _config.web_search ? "on" : "off" ) + "\n";
        s += "preview:   " + ( _config.paste_preview == 0
                 ? std::string("all lines")
                 : "first " + std::to_string(_config.paste_preview) + " lines" ) + "\n";
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
        auto mode = [this]() -> std::string {
            if ( !_config.thinking_stream ) return "off";
            return _config.thinking_collapse ? "collapse" : "on";
        };
        std::string m = common::to_lower(args);
        if ( m.empty())
            return "stream: " + mode();
        if ( m == "on" || m == "true" ) { _config.thinking_stream = true; _config.thinking_collapse = false; }
        else if ( m == "off" || m == "false" ) { _config.thinking_stream = false; _config.thinking_collapse = false; }
        else if ( m == "collapse" || m == "hide" ) { _config.thinking_stream = true; _config.thinking_collapse = true; }
        else return "usage: /stream <off|on|collapse>";
        return "stream: " + mode();
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

    if ( cmd == "/clear" || cmd == "/reset" ) {
        _conversation.clear();
        _conversation.set_system(base_system_prompt());
        save_conversation();
        return "conversation history cleared";
    }

    if ( cmd == "/compact" ) {
        try {
            return compact_history();
        } catch ( const std::exception& e ) {
            return std::string("compact failed: ") + e.what();
        }
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
