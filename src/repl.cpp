#include "agent/repl.hpp"

#include <iostream>
#include <unistd.h>
#include <cctype>
#include <ctime>
#include <algorithm>
#include <vector>
#include <set>
#include <thread>
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
#include "agent/tools/fetch_url.hpp"
#include "agent/tools/mcp_tool.hpp"
#include "agent/tools/ask_user.hpp"
#include "agent/tools/tasks_tool.hpp"
#include "agent/tools/skill_tool.hpp"
#include "agent/skills.hpp"
#include "agent/commands.hpp"
#include "agent/tools/run_command.hpp"

namespace agent {

// Render a streamed Response back into a readable blob for /raw (there is no raw
// JSON body to keep in the streaming path — it was assembled from SSE deltas).
static std::string raw_response_dump(const providers::Response& r) {
    std::string s = "(assembled from stream)\n";
    if ( !r.message.empty()) s += "\ncontent:\n" + r.message + "\n";
    if ( !r.thinking.empty()) s += "\nreasoning:\n" + r.thinking + "\n";
    for ( const auto& tc : r.tool_calls )
        s += "\ntool_call " + tc.name + ":\n" + tc.arguments.dump() + "\n";
    s += "\nusage: input=" + std::to_string(r.input_tokens) +
         " output=" + std::to_string(r.output_tokens);
    if ( r.cached_input_tokens ) s += " cached=" + std::to_string(r.cached_input_tokens);
    return s;
}

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

    // User-config extensions to the command safety lists (config file only).
    tools::Registry::set_extra_safe(_config.tools_safe);
    tools::Registry::set_extra_danger(_config.tools_danger);

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
    reload_skills();      // discover skills on disk
    sync_skill_tool();    // expose use_skill when any exist
    connect_mcp();

    // Snapshot files before write_file overwrites them, for /changes + revert.
    _registry.set_pre_run_callback([this](const std::string& n, const JSON& a) {
        record_file_change(n, a);
    });

    // A todo list the model maintains (update_tasks tool), shown by /tasks.
    if ( _config.tools_enabled )
        _registry.add(std::make_unique<tools::TasksTool>(
            [this](const JSON& t) { return set_tasks(t); }));
}

std::string Repl::set_tasks(const JSON& tasks) {
    _tasks.clear();
    size_t done = 0, in_progress = 0;
    for ( size_t i = 0; i < tasks.size(); ++i ) {
        JSON t = tasks[i];
        std::string title = t.contains("title") ? common::trim_ws(t["title"].to_string()) : "";
        if ( title.empty())
            continue;
        std::string status = t.contains("status") ? common::to_lower(t["status"].to_string()) : "pending";
        if ( status != "done" && status != "in_progress" && status != "pending" )
            status = "pending";
        if ( status == "done" ) ++done;
        else if ( status == "in_progress" ) ++in_progress;
        _tasks.push_back({ title, status });
    }
    return "tasks updated: " + std::to_string(_tasks.size()) + " (" +
           std::to_string(done) + " done, " + std::to_string(in_progress) + " in progress)";
}

std::string Repl::shell_passthrough(const std::string& cmd) {
    if ( cmd.empty())
        return "usage: !<shell command>";
    tools::RunCommand rc;
    std::string out = rc.execute(JSON::Object{ { "command", cmd } });
    // Record what the user did and saw, so "fix those" just works next turn.
    _conversation.add_user("(I ran this shell command myself)\n$ " + cmd + "\n" + out);
    save_conversation();
    return out;
}

std::string Repl::tasks_command() const {
    if ( _tasks.empty())
        return "no tasks yet (the model tracks them with the update_tasks tool during multi-step work)";
    std::string s = "tasks:\n";
    for ( const auto& t : _tasks ) {
        std::string glyph = t.status == "done" ? "✓" : ( t.status == "in_progress" ? "▸" : "○" );
        s += "\n  " + glyph + " " + t.title;
    }
    return s;
}

void Repl::record_file_change(const std::string& tool, const JSON& args) {
    if (( tool != "write_file" && tool != "edit_file" ) || args != JSON::TYPE::OBJECT ||
        !args.contains("path"))
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
        return agent::block_diff(it->second.original, cur.value_or(""), "original", "current");
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
    // Keep exports inside the home or working directory — never let a path
    // (which the model can propose) overwrite an arbitrary system file.
    {
        std::error_code ec;
        std::string ap = std::filesystem::weakly_canonical(std::filesystem::absolute(path, ec), ec).string();
        if ( ap.empty()) ap = std::filesystem::absolute(path, ec).string();
        auto within = [&](const std::string& base) {
            if ( base.empty()) return false;
            std::error_code bec;
            std::string b = std::filesystem::weakly_canonical(std::filesystem::absolute(base, bec), bec).string();
            return !b.empty() && ( ap == b || ap.rfind(b + "/", 0) == 0 );
        };
        std::string cwd;
        try { cwd = std::filesystem::current_path().string(); } catch ( ... ) {}
        if ( !within(_config.home_dir) && !within(cwd))
            return "error: /export path must be inside your home or the working directory";
    }

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
        n = _mcp.load_config(_config.mcp_config, /*trusted=*/true);
    } else {
        n += _mcp.load_config(_config.home_dir + "/mcp.json", /*trusted=*/true);
        // A project's ./.mcp.json is untrusted — anyone who shares the repo can
        // write it, and a stdio server runs an arbitrary binary. Load it, but
        // don't spawn until the user approves (approve_project_mcp, once the
        // confirm UI exists).
        n += _mcp.load_config(".mcp.json", /*trusted=*/false);
    }
    if ( n == 0 )
        return;

    _mcp.connect_all();
    register_mcp_tools();
}

// Prompt for each untrusted (project-local) MCP server and connect the approved
// ones. Called from run_tty once the confirm callback is wired.
void Repl::approve_project_mcp(const std::function<bool(const std::string&, const std::string&)>& ask) {
    auto pending = _mcp.pending_approvals();
    if ( pending.empty())
        return;
    bool any = false;
    for ( const auto& [name, what] : pending ) {
        if ( ask(name, what) && _mcp.approve_connect(name))
            any = true;
    }
    if ( any )
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
            }, t.read_only, t.destructive));
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
            }, /*read_only=*/true));
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
    // web_search and fetch_url are both network tools, gated by the same toggle.
    bool want = _config.tools_enabled && _config.web_search;
    if ( want && !_registry.has("web_search"))
        _registry.add(std::make_unique<tools::WebSearch>(_config.web_search_url));
    else if ( !want && _registry.has("web_search"))
        _registry.remove("web_search");
    if ( want && !_registry.has("fetch_url"))
        _registry.add(std::make_unique<tools::FetchUrl>());
    else if ( !want && _registry.has("fetch_url"))
        _registry.remove("fetch_url");
}

void Repl::sync_workflow_tool() {
    // The run_workflow tool is available when tools are on and the provider can
    // drive background sub-agents (claude). Kept in sync across /provider switches.
    bool want = _config.tools_enabled && provider_supports("workflows");
    if ( want && !_registry.has("run_workflow")) {
        _registry.add(std::make_unique<tools::WorkflowTool>(
            [this](const std::string& name, const std::vector<std::string>& steps, bool parallel) {
                Config cfg = _config; // snapshot: sub-agents must not read live config
                int id = _workflows.launch(name, steps,
                    [cfg](const std::string& task, std::atomic<bool>* abort) {
                        return run_workflow_step(cfg, task, abort);
                    }, parallel);
                return "started workflow #" + std::to_string(id) + " (" +
                       std::to_string(steps.size()) + " step(s), " +
                       ( parallel ? "parallel" : "serial" ) + ") in the background; "
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

    // Subcommands: cancel <id> stops a running workflow, retry <id> relaunches a
    // finished one (already-succeeded steps keep their results and are skipped).
    {
        std::istringstream iss(a);
        std::string sub, rest;
        iss >> sub >> rest;
        std::string lsub = common::to_lower(sub);
        if ( lsub == "cancel" || lsub == "retry" ) {
            int id = 0;
            try { id = std::stoi(rest); } catch ( ... ) { return "usage: /workflows " + lsub + " <id>"; }
            if ( lsub == "cancel" )
                return _workflows.cancel(id)
                     ? "cancelling workflow #" + rest + " (its running step is aborted)"
                     : "no running workflow #" + rest;
            Config cfg = _config;
            int nid = _workflows.retry(id, [cfg](const std::string& task, std::atomic<bool>* abort) {
                return run_workflow_step(cfg, task, abort);
            });
            if ( nid < 0 )
                return "cannot retry #" + rest + " (unknown, still running, or every step succeeded)";
            return "retrying workflow #" + rest + " as #" + std::to_string(nid) +
                   " — steps that already succeeded are kept";
        }
    }

    if ( !a.empty()) {
        // Detail view for one run.
        int want = 0;
        try { want = std::stoi(a); } catch ( ... ) { return "usage: /workflows [id|cancel <id>|retry <id>]"; }
        for ( const auto& r : runs ) {
            if ( r.id != want ) continue;
            std::string s = "workflow #" + std::to_string(r.id) + "  " + r.name +
                            "  [" + r.status + "]" + ( r.parallel ? "  (parallel)" : "" ) + "\n";
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
             "  [" + r.status + "]" + ( r.parallel ? " (parallel)" : "" ) + "  " +
             std::to_string(done) + "/" + std::to_string(r.steps.size()) + " steps";
    }
    s += "\n\nuse /workflows <id> for details, cancel <id> / retry <id> to manage";
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

    // User-pinned notes: kept verbatim so they survive compaction.
    if ( !_pins.empty()) {
        system += "\n\n## Pinned context (keep these in mind; do not forget)\n\n";
        for ( const auto& p : _pins )
            system += "- " + p + "\n";
    }

    // Plan mode: tell the model to plan, not act (its mutating tools are blocked).
    if ( _config.plan_mode )
        system += "\n\n## Plan mode\n\nYou are in PLAN MODE. Investigate with the "
                  "read-only tools and PRESENT A PLAN — do not modify files or run "
                  "commands (those tools are disabled and will refuse). Wait for the user "
                  "to review and turn plan mode off before you make any changes.";

    // Active skills: their full instructions, so they persist across compaction.
    for ( const auto& s : _skills ) {
        if ( !_active_skills.count(s.name))
            continue;
        system += "\n\n## Skill: " + s.name;
        if ( !s.description.empty())
            system += " — " + s.description;
        system += "\n\n" + s.content + "\n";
    }
    return system;
}

void Repl::reload_skills() {
    std::string cwd;
    try { cwd = std::filesystem::current_path().string(); } catch ( ... ) { cwd = "."; }
    _skills = load_skills(_config.home_dir, cwd);
    // Drop active names that no longer exist on disk.
    for ( auto it = _active_skills.begin(); it != _active_skills.end(); ) {
        bool found = false;
        for ( const auto& s : _skills ) if ( s.name == *it ) { found = true; break; }
        it = found ? std::next(it) : _active_skills.erase(it);
    }
}

std::string Repl::skills_command() const {
    if ( _skills.empty())
        return "no skills found — add markdown files to " + _config.home_dir +
               "/skills/ or ./.agent/skills/ (optional frontmatter: name, description)";
    std::string s = "skills:\n";
    for ( const auto& sk : _skills ) {
        bool on = _active_skills.count(sk.name) > 0;
        s += "\n  " + std::string(on ? "●" : "○") + " " + sk.name +
             " [" + sk.source + "]";
        if ( !sk.description.empty())
            s += " — " + sk.description;
    }
    s += "\n\n/skill <name> to activate, /skill off <name> to deactivate";
    return s;
}

std::string Repl::activate_skill(const std::string& name) {
    for ( const auto& sk : _skills ) {
        if ( sk.name != name )
            continue;
        _active_skills.insert(name);
        _conversation.set_system(base_system_prompt()); // take effect immediately
        return "skill '" + name + "' activated:\n\n" + sk.content;
    }
    // Not found: give the model/user the available names.
    std::string names;
    for ( const auto& sk : _skills )
        names += ( names.empty() ? "" : ", " ) + sk.name;
    return "error: no skill named '" + name + "'" +
           ( names.empty() ? "" : " (available: " + names + ")" );
}

std::string Repl::skill_command(const std::string& args) {
    std::string a = common::trim_ws(args);
    if ( a.empty())
        return "usage: /skill <name>   |   /skill off <name>";
    std::istringstream iss(a);
    std::string first; iss >> first;
    if ( common::to_lower(first) == "off" ) {
        std::string name; { std::string r; std::getline(iss, r); name = common::trim_ws(r); }
        if ( name.empty())
            return "usage: /skill off <name>";
        if ( !_active_skills.erase(name))
            return "skill '" + name + "' was not active";
        _conversation.set_system(base_system_prompt());
        return "skill '" + name + "' deactivated";
    }
    std::string res = activate_skill(a);
    // For the user, a short confirmation is enough (the instructions are now in
    // context); the full body is only useful to the model via use_skill.
    if ( res.rfind("skill '", 0) == 0 )
        return "activated skill '" + a + "' (its instructions are now in context; /skill off " + a + " to remove)";
    return res;
}

std::string Repl::skill_tool_description() const {
    std::string d = "Load a skill: a reusable, named instruction set for a kind of task. "
                    "Call this when one of the available skills fits what the user asked, "
                    "then follow its instructions. Available skills:";
    for ( const auto& sk : _skills )
        d += "\n- " + sk.name + ( sk.description.empty() ? "" : ": " + sk.description );
    return d;
}

void Repl::sync_skill_tool() {
    bool want = _config.tools_enabled && !_skills.empty();
    if ( want && !_registry.has("use_skill")) {
        _registry.add(std::make_unique<tools::SkillTool>(
            [this]() { return skill_tool_description(); },
            [this](const std::string& name) { return activate_skill(name); }));
    } else if ( !want && _registry.has("use_skill")) {
        _registry.remove("use_skill");
    }
}

std::string Repl::pin_command(const std::string& args) {
    std::string text = common::trim_ws(args);
    if ( text.empty()) {
        // No text: pin the most recent assistant reply.
        const auto& msgs = _conversation.messages();
        for ( auto it = msgs.rbegin(); it != msgs.rend(); ++it )
            if ( it->role == Role::ASSISTANT && !it->content.empty()) { text = it->content; break; }
        if ( text.empty())
            return "usage: /pin <text>   (or /pin with no text to pin the last reply)";
    }
    _pins.push_back(text);
    _conversation.set_system(base_system_prompt());   // take effect immediately
    std::string preview = text.substr(0, 70);
    if ( text.size() > 70 ) preview += "…";
    return "pinned #" + std::to_string(_pins.size()) + " (kept through /compact): " + preview;
}

std::string Repl::pins_command() const {
    if ( _pins.empty())
        return "no pinned notes — /pin <text> keeps a note in context through compaction";
    std::string s = "pinned notes:\n";
    for ( size_t i = 0; i < _pins.size(); ++i ) {
        std::string p = _pins[i];
        if ( p.size() > 200 ) p = p.substr(0, 200) + "…";
        s += "\n  " + std::to_string(i + 1) + ". " + p;
    }
    s += "\n\n/unpin <n> to remove one";
    return s;
}

std::string Repl::unpin_command(const std::string& args) {
    std::string a = common::trim_ws(args);
    if ( a == "all" ) {
        size_t n = _pins.size();
        _pins.clear();
        _conversation.set_system(base_system_prompt());
        return "removed " + std::to_string(n) + " pin(s)";
    }
    int n = 0;
    try { n = std::stoi(a); } catch ( ... ) { return "usage: /unpin <n|all>  (see /pins)"; }
    if ( n < 1 || n > static_cast<int>(_pins.size()))
        return "no pin #" + a + " (see /pins)";
    _pins.erase(_pins.begin() + ( n - 1 ));
    _conversation.set_system(base_system_prompt());
    return "removed pin #" + a + " (" + std::to_string(_pins.size()) + " left)";
}

std::string Repl::compact_history(size_t keep_tail) {
    const auto& msgs = _conversation.messages();
    size_t count = 0;
    for ( const auto& m : msgs )
        if ( m.role != Role::SYSTEM ) ++count;
    if ( count < 4 )
        return "nothing to compact (conversation is already short)";

    // Rolling compaction: keep the last `keep_tail` user exchanges verbatim and
    // only summarise what comes before. Find the tail boundary at a user message.
    size_t n = msgs.size();
    size_t first_nonsys = ( n > 0 && msgs[0].role == Role::SYSTEM ) ? 1 : 0;
    size_t tail_start = n;
    if ( keep_tail > 0 ) {
        size_t seen = 0;
        for ( size_t i = n; i-- > first_nonsys; ) {
            if ( msgs[i].role == Role::USER && ++seen == keep_tail ) { tail_start = i; break; }
        }
        if ( seen < keep_tail )
            tail_start = first_nonsys; // fewer than K exchanges — nothing old to summarise
    }
    size_t old_count = ( tail_start > first_nonsys ) ? ( tail_start - first_nonsys ) : 0;
    if ( old_count < 2 )
        return "nothing to compact (the recent tail is the whole conversation)";

    // Copy the verbatim tail before we rebuild the history below.
    std::vector<Message> tail(msgs.begin() + tail_start, msgs.end());

    // Render only the OLD part for the summariser.
    std::string transcript;
    for ( size_t i = first_nonsys; i < tail_start; ++i ) {
        const auto& m = msgs[i];
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

    // Rough progress estimate: a summary runs ~1/12 of the input length, bounded.
    // Not exact, but enough for a climbing bar ("coffee or nap?").
    size_t expected = std::min<size_t>(std::max<size_t>(( transcript.size() / 4 ) / 12, 200), 900);
    auto bar = [](int pct) {
        int filled = pct * 12 / 100;
        std::string b = "[";
        for ( int i = 0; i < 12; ++i ) b += ( i < filled ? "█" : "░" );
        return b + "]";
    };

    std::string summary;
    if ( _provider->supports_streaming()) {
        req["stream"] = true;
        std::string body = req.dump_minified();
        std::string buffer;
        bool done = false;
        size_t got_chars = 0;
        _provider->stream_reset();
        _client.post_stream(_provider->endpoint(), _provider->auth_header(),
            _provider->auth_value(), _provider->extra_headers(), body,
            [&](const std::string& chunk) {
                providers::StreamChunk sc = _provider->parse_stream(chunk, buffer, done);
                got_chars += sc.content.size();
                int pct = expected ? static_cast<int>(std::min<size_t>(99, ( got_chars / 4 ) * 100 / expected)) : 0;
                if ( _progress_cb )
                    _progress_cb("compacting " + bar(pct) + " ~" + std::to_string(pct) + "%");
            }, &agent::turn_abort);
        if ( agent::turn_abort.load(std::memory_order_relaxed))
            return "compact cancelled";
        summary = agent::normalize_text(_provider->stream_result().message);
    } else {
        std::string body = req.dump_minified();
        std::string resp_str = _client.post(_provider->endpoint(), _provider->auth_header(),
                                            _provider->auth_value(), _provider->extra_headers(), body,
                                            &agent::turn_abort);
        if ( agent::turn_abort.load(std::memory_order_relaxed) || resp_str.empty())
            return "compact cancelled";
        auto resp = _provider->parse_response(JSON::parse(resp_str));
        if ( !resp.success )
            throws << "summarisation failed: " << resp.message << std::endl;
        summary = agent::normalize_text(resp.message);
    }
    if ( summary.empty())
        throws << "summarisation returned no content" << std::endl;

    // Carry the machine-held session state verbatim (do not trust the LLM summary
    // to have mentioned it) — the same anchoring trick /pin uses for user notes.
    std::string state;
    if ( !_tasks.empty()) {
        state += "\n\nTask list at this point:\n";
        for ( const auto& t : _tasks )
            state += "- [" + t.status + "] " + t.title + "\n";
    }
    if ( !_changes.empty()) {
        state += "\n\nFiles changed this session (already applied):\n";
        for ( const auto& [path, fc] : _changes )
            state += "- " + path + ( fc.existed ? "" : " (created)" ) + "\n";
    }

    // Rebuild: [system, user(summary+state), assistant(ack), …verbatim tail…].
    // The tail begins at a user message, so alternation stays valid.
    _conversation.clear();
    _conversation.set_system(base_system_prompt());
    _conversation.add_user("Summary of the earlier part of our conversation — use this as "
                           "context; the most recent messages follow verbatim:\n\n" + summary + state);
    _conversation.add_assistant("Understood — I'll continue with that context in mind.");
    for ( const auto& m : tail ) {
        if ( m.role == Role::USER )
            _conversation.add_user(m.content);
        else if ( m.role == Role::ASSISTANT )
            _conversation.add_assistant(m.content, m.tool_calls, m.thinking_blocks);
        else if ( m.role == Role::TOOL )
            _conversation.add_tool_result(m.tool_call_id.value_or(""), m.name.value_or(""), m.content);
    }
    save_conversation();

    return "compacted " + std::to_string(old_count) + " older messages into a summary" +
           ( tail.empty() ? "" : " (kept the last " + std::to_string(tail.size()) + " verbatim)" );
}

std::string Repl::switch_provider(const std::string& name) {
    static const std::vector<std::string> supported =
        { "openai", "ollama", "anthropic", "moonshot", "openrouter", "kimi", "claude" };
    if ( std::find(supported.begin(), supported.end(), name) == supported.end())
        return "unknown provider: " + name +
               "  (openai, ollama, anthropic, moonshot, openrouter, kimi, claude)";
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
    _workflow_autoresume.store(_config.workflow_autoresume, std::memory_order_relaxed);
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
    _registry.begin_turn(); // reset any "allow for the rest of this turn" grant

    // "ultracode" / "ultrathink": for this one turn, raise the Anthropic thinking
    // effort to max. The keyword is intentionally kept in the prompt (the model
    // reacts to it — e.g. by orchestrating a workflow), and the previous effort is
    // restored afterwards on every exit path via the guard below.
    bool anthropic_based = ( _config.provider == "claude" || _config.provider == "anthropic" );
    bool ultra = anthropic_based && _provider && has_ultra_keyword(prompt);
    std::string saved_effort = _config.thinking;
    if ( ultra )
        _provider->apply_provider_options(JSON::Object{ { "thinking", "max" } });
    struct EffortRestore {
        providers::Provider* p; bool on; std::string to;
        ~EffortRestore() {
            if ( on && p )
                p->apply_provider_options(JSON::Object{ { "thinking", to.empty() ? "off" : to } });
        }
    } effort_restore{ _provider.get(), ultra, saved_effort };

    // Whether a dim "thinking" region is currently open in the streamed output.
    // Tracked across tool-loop iterations so the answer after a tool call is not
    // left rendered as dim reasoning.
    bool showing_thinking = false;
    size_t tools_this_turn = 0;                        // per-turn tool-call budget counter
    size_t next_tool_check = _config.tool_call_limit;  // ask again at each multiple

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

        // The network call, retried on transient errors (429/503/529) with
        // exponential backoff — but only while nothing has been streamed yet, so
        // a mid-stream failure never double-renders. `produced` tracks that.
        bool produced = false;
        for ( int attempt = 0; ; ++attempt ) {
            try {
                if ( can_stream ) {
                    request["stream"] = true;
                    _provider->prepare_stream_request(request); // e.g. stream_options.include_usage
                    _last_request = request.dump(); // for /raw
                    std::string body = request.dump_minified();
                    std::string buffer;
                    bool done = false;
                    _provider->stream_reset();

                    _client.post_stream(_provider->endpoint(), _provider->auth_header(), _provider->auth_value(), headers, body,
                        [&](const std::string& chunk) {
                            logger::vverbose["http"] << "STREAM chunk\n" << chunk << std::endl;
                            providers::StreamChunk sc = _provider->parse_stream(chunk, buffer, done);
                            if ( _config.thinking_stream && !sc.reasoning.empty()) {
                                if ( !showing_thinking ) { stream_cb("\n\x01"); showing_thinking = true; }
                                produced = true;
                                stream_cb(sc.reasoning);
                            }
                            if ( !sc.content.empty()) {
                                if ( showing_thinking ) { stream_cb("\n\x02\n\n"); showing_thinking = false; }
                                produced = true;
                                stream_cb(agent::normalize_text(sc.content));
                            }
                        }, abort_flag);

                    if ( abort_flag && abort_flag->load(std::memory_order_relaxed)) {
                        _conversation.undo_last();
                        return "";
                    }
                    resp = _provider->stream_result();
                    _last_response = raw_response_dump(resp); // for /raw (assembled from the stream)
                } else {
                    _last_request = request.dump(); // for /raw
                    std::string body = request.dump_minified();
                    std::string response_str = _client.post(_provider->endpoint(), _provider->auth_header(), _provider->auth_value(), headers, body, abort_flag);
                    if ( abort_flag && abort_flag->load(std::memory_order_relaxed)) {
                        _conversation.undo_last();
                        return "";
                    }
                    _last_response = response_str; // for /raw (the raw JSON body)
                    resp = _provider->parse_response(JSON::parse(response_str));
                }
                break; // success
            } catch ( const std::exception& e ) {
                std::string m = e.what();
                bool transient = m.find("http error") != std::string::npos &&
                    ( m.find(" 429") != std::string::npos || m.find(" 503") != std::string::npos ||
                      m.find(" 529") != std::string::npos );
                bool aborting = abort_flag && abort_flag->load(std::memory_order_relaxed);
                if ( !transient || produced || aborting || attempt >= 3 )
                    throw;
                // Backoff 0.5s, 1s, 2s — interruptible via the abort flag.
                long ms = 500L << attempt;
                if ( _progress_cb )
                    _progress_cb("provider busy — retrying in " + std::to_string(ms / 1000.0).substr(0, 3) + "s");
                for ( long slept = 0; slept < ms; slept += 100 ) {
                    if ( abort_flag && abort_flag->load(std::memory_order_relaxed)) { _conversation.undo_last(); return ""; }
                    struct timespec ts { 0, 100L * 1000 * 1000 };
                    nanosleep(&ts, nullptr);
                }
            }
        }

        _stats.record(resp.input_tokens, resp.output_tokens, resp.cached_input_tokens);

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
            _conversation.add_assistant(normalized, assistant_calls, resp.thinking_blocks);

        if ( resp.tool_calls.empty()) {
            // Warn when the reply was cut off by the output-token cap (rather than
            // finishing) — otherwise a truncated file/answer looks complete.
            std::string trunc;
            if ( resp.truncated )
                trunc = "\n\n⚠ reply truncated at the output-token cap (max_tokens=" +
                        std::to_string(_config.max_tokens ? _config.max_tokens : 8192) +
                        "). Raise it with `/settings max_tokens <n>` or ask me to continue.";
            // In the streaming path the reasoning and answer already went to the
            // live callback. Only the non-streaming fallback prepends the reasoning
            // block here (display only — the saved message keeps just the answer).
            if ( can_stream ) {
                if ( !trunc.empty() && stream_cb )
                    stream_cb(trunc);
                return normalized;
            }
            if ( !resp.thinking.empty())
                return "💭 " + agent::normalize_text(resp.thinking) + "\n\n" + normalized + trunc;
            return normalized + trunc;
        }

        // Run the tool calls. When the model batches several read-only tools
        // (the common "read these files / grep these" case) they run concurrently;
        // anything that writes, runs a command or has side effects stays serial.
        static const std::set<std::string> parallel_safe = {
            "read_file", "grep", "find_symbol", "list_directory"
        };
        bool run_parallel = _config.parallel_tools && resp.tool_calls.size() > 1;
        for ( const auto& tc : resp.tool_calls )
            if ( !parallel_safe.count(tc.name)) { run_parallel = false; break; }

        std::vector<std::string> results(resp.tool_calls.size());
        // The most descriptive string argument, for the one-line transcript note.
        auto arg_hint = [](const JSON& args) -> std::string {
            if ( args != JSON::TYPE::OBJECT )
                return "";
            for ( const char* k : { "path", "command", "pattern", "symbol", "query", "url", "name" }) {
                if ( args.contains(k) && args[k] == JSON::TYPE::STRING ) {
                    std::string v = args[k].to_string();
                    for ( char& c : v )
                        if ( c == '\n' || c == '\t' ) c = ' ';
                    if ( v.size() > 48 ) v = v.substr(0, 48) + "…";
                    return v;
                }
            }
            return "";
        };
        // A short summary of what a tool call produced, shown in its ⚙ notice:
        // grep "12 matches", read_file "340 lines", edit_file "+5/-2 lines", … so
        // the transcript shows the shape of each result without opening it.
        auto result_hint = [](const std::string& name, const JSON& args,
                              const std::string& result) -> std::string {
            if ( result.rfind("error:", 0) == 0 )
                return ""; // the error is already appended to the notice
            auto line_count = [](const std::string& s) -> size_t {
                if ( s.empty()) return 0;
                size_t n = static_cast<size_t>(std::count(s.begin(), s.end(), '\n'));
                return s.back() == '\n' ? n : n + 1;
            };
            // These tools already lead with a count summary ("12 matches", …).
            if ( name == "grep" || name == "find_symbol" ||
                 name == "find_references" || name == "outline_file" ) {
                std::string first = result.substr(0, result.find('\n'));
                if ( !first.empty() && std::isdigit(static_cast<unsigned char>(first[0]))) {
                    if ( first.back() == ':' ) first.pop_back();
                    return first.size() > 40 ? first.substr(0, 40) + "…" : first;
                }
                return "";
            }
            if ( name == "read_file" )
                return std::to_string(line_count(result)) + " lines";
            if ( name == "list_directory" )
                return std::to_string(line_count(result)) + " entries";
            if ( name == "write_file" ) {
                std::string tail = result.rfind("ok: ", 0) == 0 ? result.substr(4) : result;
                size_t nl = tail.find('\n');
                return nl == std::string::npos ? tail : tail.substr(0, nl);
            }
            if ( name == "edit_file" ) {
                size_t added = 0, removed = 0;
                auto tally = [&](const JSON& e) {
                    if ( e.contains("old_string") && e["old_string"] == JSON::TYPE::STRING )
                        removed += line_count(e["old_string"].to_string());
                    if ( e.contains("new_string") && e["new_string"] == JSON::TYPE::STRING )
                        added += line_count(e["new_string"].to_string());
                };
                if ( args.contains("edits") && args["edits"] == JSON::TYPE::ARRAY ) {
                    const JSON& es = args["edits"];
                    for ( size_t k = 0; k < es.size(); ++k ) tally(es[k]);
                } else {
                    tally(args);
                }
                return "+" + std::to_string(added) + "/-" + std::to_string(removed) + " lines";
            }
            return "";
        };
        auto run_one = [&](size_t i) {
            auto t0 = std::chrono::steady_clock::now();
            try {
                results[i] = _registry.execute(resp.tool_calls[i].name, resp.tool_calls[i].arguments);
            } catch ( const std::exception& e ) {
                results[i] = std::string("error: ") + e.what();
            }
            if ( _tool_notice_cb ) {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                char dur[24];
                std::snprintf(dur, sizeof(dur), "%.1fs", ms / 1000.0);
                std::string hint = arg_hint(resp.tool_calls[i].arguments);
                std::string rhint = result_hint(resp.tool_calls[i].name,
                                                 resp.tool_calls[i].arguments, results[i]);
                std::string line = "⚙ " + resp.tool_calls[i].name +
                                   ( hint.empty() ? "" : " " + hint ) +
                                   ( rhint.empty() ? "" : " · " + rhint ) + " · " + dur;
                if ( results[i].rfind("error:", 0) == 0 ) {
                    std::string err = results[i].substr(0, 80);
                    size_t nl = err.find('\n');
                    if ( nl != std::string::npos ) err.resize(nl);
                    line += " · " + err;
                }
                _tool_notice_cb(line);
            }
        };

        if ( run_parallel ) {
            std::atomic<size_t> next{ 0 };
            size_t nthreads = std::min<size_t>(resp.tool_calls.size(), 8);
            std::vector<std::thread> pool;
            for ( size_t t = 0; t < nthreads; ++t )
                pool.emplace_back([&]() {
                    for ( size_t i = next.fetch_add(1); i < resp.tool_calls.size(); i = next.fetch_add(1))
                        run_one(i);
                });
            for ( auto& th : pool )
                th.join();
        } else {
            for ( size_t i = 0; i < resp.tool_calls.size(); ++i )
                run_one(i);
        }

        // Record the results in the model's original order.
        for ( size_t i = 0; i < resp.tool_calls.size(); ++i ) {
            const auto& tc = resp.tool_calls[i];
            logger::info["tool"] << tc.name << " -> " << results[i].substr(0, 200) << std::endl;
            _conversation.add_tool_result(tc.id, tc.name, results[i]);
        }

        // Interrupted (e.g. Ctrl-C'd a long run_command)? Stop the turn but KEEP
        // the tool results — the command's partial output is already captured, so
        // it stays in context and a follow-up message can reason over it, instead
        // of undoing the whole exchange and throwing that log away.
        if ( abort_flag && abort_flag->load(std::memory_order_relaxed))
            return "(interrupted — output so far is kept; send a message to continue)";

        // Per-turn tool-call budget: a runaway-loop guard so `/tools auto` can be
        // left unattended. After every `tool_call_limit` calls, ask whether to
        // keep going; a stop ends the turn (the work so far is kept, and a new
        // message continues). Non-interactive runs stop automatically.
        tools_this_turn += resp.tool_calls.size();
        if ( _config.tool_call_limit > 0 && tools_this_turn >= next_tool_check &&
             !( abort_flag && abort_flag->load(std::memory_order_relaxed))) {
            bool go = _registry.ask_continue(
                "the model has run " + std::to_string(tools_this_turn) +
                " tool calls this turn — continue for up to " +
                std::to_string(_config.tool_call_limit) + " more, or stop?");
            if ( !go )
                return "(stopped after " + std::to_string(tools_this_turn) +
                       " tool calls this turn — the work so far is kept; send a message to continue)";
            next_tool_check += _config.tool_call_limit;
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
    // !shell passthrough: the user runs a command directly — no model turn, no
    // confirmation (they typed it themselves) — but the output is recorded so
    // the model sees it on its next turn ("!make test" … "fix those").
    if ( !line.empty() && line[0] == '!' )
        return shell_passthrough(common::trim_ws(line.substr(1)));

    std::string cmd, args;
    {
        std::istringstream iss(line);
        iss >> cmd;
        std::getline(iss, args);
        args = common::trim_ws(args);
    }

    if ( cmd == "/help" ) {
        if ( args.empty())
            return commands_overview();
        std::string detail = command_help(args);
        return detail.empty()
             ? "no such command: " + args + "  (try /help for the full list)"
             : detail;
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
        long cached = _stats.session_cached.load(std::memory_order_relaxed);
        long total = in + out;
        std::string s = "session usage (" + _config.provider + " · " + _config.model + "):\n";
        s += "  input:   " + std::to_string(in) + " tokens";
        if ( cached > 0 )
            s += " (" + std::to_string(cached) + " cached, billed ~10%)";
        s += "\n";
        s += "  output:  " + std::to_string(out) + " tokens\n";
        s += "  total:   " + std::to_string(total) + " tokens\n";

        double cost = _config.session_cost(in, out, cached);
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

    if ( cmd == "/tasks" ) {
        return tasks_command();
    }

    if ( cmd == "/trust" ) {
        std::istringstream iss(args);
        std::string sub, rest;
        iss >> sub; { std::string r; std::getline(iss, r); rest = common::trim_ws(r); }
        auto grants = _registry.grants();
        if ( common::to_lower(sub) == "drop" ) {
            if ( rest.empty())
                return "usage: /trust drop <n|all>";
            if ( common::to_lower(rest) == "all" )
                return "revoked " + std::to_string(_registry.revoke_all_grants()) + " grant(s)";
            int n = 0;
            try { n = std::stoi(rest); } catch ( ... ) { return "usage: /trust drop <n|all>"; }
            if ( n < 1 || n > static_cast<int>(grants.size()))
                return grants.empty() ? "no standing grants" : "no grant #" + rest + " (see /trust)";
            std::string key = grants[n - 1].key;
            _registry.revoke_grant(key);
            return "revoked #" + rest + ": " + key;
        }
        // Overview.
        std::string mode = _config.insecure ? "insecure (no confirmation)"
                         : ( _config.confirm_tools ? "confirm" : "auto" );
        std::string s = "trust:\n\n  mode: " + mode + ( _config.strict ? " (strict)" : "" );
        if ( _registry.turn_grant_active())
            s += "\n  turn grant: active (allowed for the rest of this turn)";
        if ( !_config.tools_safe.empty())
            s += "\n  config safe: " + common::join_vector(_config.tools_safe, ", ");
        if ( !_config.tools_danger.empty())
            s += "\n  config danger: " + common::join_vector(_config.tools_danger, ", ");
        if ( grants.empty()) {
            s += "\n\n  no standing grants this session";
        } else {
            s += "\n\n  standing grants (allow session / similar):";
            for ( size_t i = 0; i < grants.size(); ++i )
                s += "\n    " + std::to_string(i + 1) + ". [" + grants[i].kind + "] " +
                     grants[i].key + "  (used " + std::to_string(grants[i].uses) + "x)";
            s += "\n\n  /trust drop <n|all> to revoke";
        }
        return s;
    }

    if ( cmd == "/plan" ) {
        std::string m = common::to_lower(common::trim_ws(args));
        if ( m.empty())
            m = _config.plan_mode ? "off" : "on"; // bare /plan toggles
        if ( m == "on" || m == "true" || m == "1" ) _config.plan_mode = true;
        else if ( m == "off" || m == "false" || m == "0" ) _config.plan_mode = false;
        else return "usage: /plan [on|off]";
        _registry.set_plan_mode(_config.plan_mode);
        _conversation.set_system(base_system_prompt()); // the model learns the mode
        return _config.plan_mode
             ? "plan mode ON — read-only tools only; I'll investigate and propose a plan, "
               "no changes until you /plan off"
             : "plan mode OFF — I can edit files and run commands again";
    }

    if ( cmd == "/skills" ) {
        reload_skills();
        sync_skill_tool();
        return skills_command();
    }
    if ( cmd == "/skill" ) {
        reload_skills();
        std::string r = skill_command(args);
        sync_skill_tool();
        return r;
    }

    if ( cmd == "/pin" ) {
        return pin_command(args);
    }
    if ( cmd == "/pins" ) {
        return pins_command();
    }
    if ( cmd == "/unpin" ) {
        return unpin_command(args);
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
                   "\nusage: /provider <openai|ollama|anthropic|moonshot|openrouter|kimi|claude>";
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

    if ( cmd == "/raw" ) {
        std::string a = common::to_lower(common::trim_ws(args));
        if ( _last_request.empty() && _last_response.empty())
            return "no model request has been sent yet this session";
        if ( a == "request" || a == "req" )
            return _last_request.empty() ? "no request captured" : _last_request;
        if ( a == "response" || a == "resp" )
            return _last_response.empty() ? "no response captured" : _last_response;
        return "── request (last sent to " + _config.provider + ") ──\n" + _last_request +
               "\n\n── response ──\n" + _last_response;
    }

    if ( cmd == "/limits" ) {
        const auto& hs = _client.last_headers();
        if ( hs.empty())
            return "no model request has been sent yet this session";
        std::vector<std::pair<std::string, std::string>> hits;
        for ( const auto& h : hs ) {
            const std::string& k = h.first;
            if ( k.find("ratelimit") != std::string::npos ||
                 k.find("rate-limit") != std::string::npos ||
                 k.find("retry-after") != std::string::npos ||
                 k.find("quota") != std::string::npos )
                hits.push_back(h);
        }
        if ( hits.empty())
            return "the " + _config.provider + " provider returned no rate-limit headers (" +
                   std::to_string(hs.size()) + " headers seen). Subscription providers often "
                   "don't expose quota this way.";
        std::string out = "rate limits (from the last " + _config.provider + " response):\n";
        for ( const auto& h : hits )
            out += "  " + h.first + ": " + h.second + "\n";
        return out;
    }

    if ( cmd == "/info" || cmd == "/about" ) {
        return std::string("agent version ") + agent::VERSION + "\n"
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
            if ( key == "bell" ) return handle_command("/bell " + val);
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
            if ( key == "max_tokens" ) {
                if ( val.empty())
                    return "max_tokens: " + std::to_string(_config.max_tokens);
                _config.max_tokens = Config::parse_size_suffixed(val, _config.max_tokens);
                if ( _config.max_tokens < 256 ) _config.max_tokens = 256;
                return "max_tokens: " + std::to_string(_config.max_tokens);
            }
            if ( key == "tool_call_limit" || key == "tool_limit" ) {
                if ( val.empty())
                    return "tool_call_limit: " + std::to_string(_config.tool_call_limit) +
                           ( _config.tool_call_limit == 0 ? " (unlimited)" : " per turn" );
                _config.tool_call_limit = Config::parse_size_suffixed(val, _config.tool_call_limit);
                return "tool_call_limit: " + std::to_string(_config.tool_call_limit) +
                       ( _config.tool_call_limit == 0 ? " (unlimited)" : " per turn" );
            }
            if ( key == "autoresume" || key == "workflow_autoresume" ) {
                std::string v = common::to_lower(val);
                if ( v == "on" || v == "true" || v == "1" || v == "yes" ) _config.workflow_autoresume = true;
                else if ( v == "off" || v == "false" || v == "0" || v == "no" ) _config.workflow_autoresume = false;
                else return "usage: /settings autoresume <on|off>";
                _workflow_autoresume.store(_config.workflow_autoresume, std::memory_order_relaxed);
                return std::string("workflow autoresume: ") + ( _config.workflow_autoresume ? "on" : "off" ) +
                       ( _config.workflow_autoresume ? "  (a finished workflow resumes the conversation; bounded to 2 in a row)" : "" );
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
            if ( key == "prompt_cache" || key == "cache" ) {
                std::string v = common::to_lower(val);
                if ( v == "on" || v == "true" || v == "1" || v == "yes" ) _config.prompt_cache = true;
                else if ( v == "off" || v == "false" || v == "0" || v == "no" ) _config.prompt_cache = false;
                else return "usage: /settings prompt_cache <on|off>";
                return std::string("prompt_cache: ") + ( _config.prompt_cache ? "on" : "off" ) +
                       "  (Anthropic cache_control; OpenAI/Kimi cache automatically)";
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
        // The picker asks for the available models: fetch them live from the
        // provider (Ollama's local models, an OpenAI/Anthropic /models listing);
        // the active model is always first, and there's a curated fallback.
        if ( args == "--list" ) {
            std::vector<std::string> models;
            models.push_back(_config.model);
            std::vector<std::string> live = _provider ? _provider->list_models(_client)
                                                      : std::vector<std::string>{};
            for ( const auto& mdl : live )
                if ( mdl != _config.model ) models.push_back(mdl);
            std::string out;
            for ( const auto& mdl : models )
                out += mdl + "\n";
            return out;
        }
        _config.model = args;
        if ( _provider )
            _provider->set_model(args);
        // Remember this model for the current provider, so the next session on the
        // same provider resumes it (state.json models[provider]).
        Config::save_last_used(_config.home_dir, _config.provider, args);
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
    _registry.set_plan_mode(_config.plan_mode);
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
    _registry.set_plan_mode(_config.plan_mode);
        return std::string("strict: ") + ( _config.strict ? "on" : "off" ) +
               ( _config.strict ? "  (safe read-only commands now also ask)" : "" );
    }

    if ( cmd == "/autoresume" ) {
        std::string m = common::to_lower(common::trim_ws(args));
        if ( m.empty())
            return std::string("workflow autoresume: ") + ( _config.workflow_autoresume ? "on" : "off" ) +
                   ( _config.workflow_autoresume
                     ? "\nA finished background workflow resumes the conversation by itself so the model "
                       "picks up its results (bounded to 2 auto-turns per message)."
                     : "\nA finished background workflow only folds its results in on your next message. "
                       "Turn on to have the model pick them up automatically." );
        if ( m == "on" || m == "true" || m == "1" || m == "yes" ) _config.workflow_autoresume = true;
        else if ( m == "off" || m == "false" || m == "0" || m == "no" ) _config.workflow_autoresume = false;
        else return "usage: /autoresume [on|off]";
        _workflow_autoresume.store(_config.workflow_autoresume, std::memory_order_relaxed);
        _config.save_settings(_config.home_dir);
        return std::string("workflow autoresume: ") + ( _config.workflow_autoresume
             ? "on — a finished workflow will resume the conversation automatically"
             : "off" );
    }

    if ( cmd == "/bell" ) {
        std::string m = common::to_lower(common::trim_ws(args));
        static const std::vector<std::string> modes = { "never", "ask_user", "question", "attention", "always" };
        if ( m.empty())
            return "bell: " + _config.bell +
                   "\n  always    — ring on every answer + everything below"
                   "\n  attention — ring on a workflow finishing, a tool-permission prompt, or a question"
                   "\n  question  — ring when the answer is a question (or the model asks via ask_user)"
                   "\n  ask_user  — ring only when the model asks you a question via the ask_user tool"
                   "\n  never     — never ring";
        if ( std::find(modes.begin(), modes.end(), m) == modes.end())
            return "usage: /bell <never|ask_user|question|attention|always>";
        _config.bell = m;
        _config.save_settings(_config.home_dir);
        return "bell: " + _config.bell;
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
        size_t keep = 2; // rolling default: keep the last 2 exchanges verbatim
        std::string a = common::to_lower(common::trim_ws(args));
        if ( a == "all" || a == "full" )
            keep = 0;
        else if ( a.rfind("keep", 0) == 0 ) {
            try { keep = static_cast<size_t>(std::stoul(common::trim_ws(a.substr(4)))); }
            catch ( ... ) { return "usage: /compact [keep <n> | all]"; }
        } else if ( !a.empty()) {
            return "usage: /compact [keep <n> | all]";
        }
        try {
            return compact_history(keep);
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
    _registry.set_plan_mode(_config.plan_mode);
    if ( _config.tools_enabled && tool_mode() != tools::ConfirmMode::insecure ) {
        _registry.set_confirm_callback([](const tools::ConfirmRequest& req, std::string& note) -> tools::Decision {
            if ( !req.danger.empty())
                std::cout << "\n⚠ dangerous command — " << req.danger << std::endl;
            std::cout << "\n" << req.tool << " wants to run:\n" << req.summary << "\n";
            std::cout << "[y] once  [s] session";
            if ( req.can_similar )
                std::cout << "  [a] all `" << req.similar_key << "`";
            std::cout << "  [N] deny  [d] deny+reason\nchoice: " << std::flush;

            std::string answer;
            if ( !std::getline(std::cin, answer))
                return tools::Decision::deny;
            std::string a = common::to_lower(common::trim_ws(answer));
            if ( a == "y" || a == "yes" || a == "once" ) return tools::Decision::once;
            if ( a == "s" || a == "session" )            return tools::Decision::session;
            if ( ( a == "a" || a == "all" ) && req.can_similar ) return tools::Decision::similar;
            if ( a == "d" ) {
                std::cout << "reason: " << std::flush;
                std::getline(std::cin, note);
                note = common::trim_ws(note);
            }
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
    _registry.set_plan_mode(_config.plan_mode);
    if ( _config.tools_enabled && tool_mode() != tools::ConfirmMode::insecure ) {
        _registry.set_confirm_callback([&inline_repl](const tools::ConfirmRequest& req, std::string& note) { return inline_repl.confirm(req, note); });
    }
    if ( _config.tools_enabled ) {
        _registry.set_activity_callback([&inline_repl](const std::string& a) { inline_repl.set_activity(a); });
    }
    // Once the terminal is in raw mode (inside run()), ask before spawning any
    // project-local MCP server — the confirm dialog needs the raw-mode terminal.
    inline_repl.set_on_ready([this, &inline_repl]() {
        approve_project_mcp([&inline_repl](const std::string& name, const std::string& what) {
            tools::ConfirmRequest req;
            req.tool = "start MCP server '" + name + "'";
            req.summary = "./.mcp.json (project-local) wants to start: " + what;
            req.danger = "a project file wants to run a program — only allow if you trust this repo";
            req.can_similar = false;
            std::string note;
            return inline_repl.confirm_on_main(req, note) != tools::Decision::deny;
        });
    });
    inline_repl.set_command_callback([this](const std::string& cmd) { return handle_command(cmd); });
    set_progress_callback([&inline_repl](const std::string& s) { inline_repl.set_activity(s); });
    set_tool_notice_callback([&inline_repl](const std::string& s) { inline_repl.notify_tool(s); });

    // ask_user tool: the model can pause and ask the user a question through the
    // interactive terminal. Only available in the interactive REPL.
    if ( _config.tools_enabled ) {
        _registry.remove("ask_user");
        _registry.add(std::make_unique<tools::AskUser>(
            [&inline_repl](const std::string& q, const std::vector<std::string>& opts) {
                return inline_repl.ask_user(q, opts);
            }));
    }

    // Workflow completion notice: printed above the live block with a bell, so a
    // long-running background run announces itself even while you're idle. With
    // workflow_autoresume on, a synthetic prompt joins the normal pending queue
    // (never a direct turn start from this background thread) so the model picks
    // the results up by itself — bounded to 2 auto turns per real user message.
    _workflow_autoresume.store(_config.workflow_autoresume, std::memory_order_relaxed);
    _workflows.set_on_finish([this, &inline_repl](const WorkflowRun& r) {
        size_t ok = 0;
        for ( const auto& st : r.steps )
            if ( st.status == "done" ) ++ok;
        std::string head = "workflow #" + std::to_string(r.id) + " " + r.name +
                           " [" + r.status + "] " + std::to_string(ok) + "/" +
                           std::to_string(r.steps.size()) + " steps";
        bool resumed = false;
        if ( _workflow_autoresume.load(std::memory_order_relaxed))
            resumed = inline_repl.enqueue_prompt(
                "Workflow #" + std::to_string(r.id) + " (" + r.name + ") just finished with status \"" +
                r.status + "\". Its step results have been delivered above — go through them and continue "
                "the task accordingly.");
        inline_repl.notify(head + ( resumed
            ? " — picking the results up automatically"
            : " — results fold in on your next message (/workflows " + std::to_string(r.id) +
              " for details · /autoresume on to pick up automatically)" ));
    });

    inline_repl.run();

    // inline_repl is about to go out of scope; synchronously detach the callbacks
    // so a run finishing right now cannot touch a dead object.
    _workflows.set_on_finish(nullptr);
    set_tool_notice_callback(nullptr);
    set_progress_callback(nullptr);
    _registry.remove("ask_user"); // its callback captured the now-dead inline_repl

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
    _registry.set_plan_mode(_config.plan_mode);

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
