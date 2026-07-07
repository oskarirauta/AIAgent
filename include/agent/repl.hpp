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
#include "agent/workflow.hpp"
#include "agent/mcp/client.hpp"

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

    // Pinned context: user-flagged notes kept verbatim in the system prompt, so
    // they survive /compact and auto-compact (which rebuild the system prompt).
    std::vector<std::string> _pins;
    std::string pin_command(const std::string& args);   // /pin
    std::string pins_command() const;                    // /pins
    std::string unpin_command(const std::string& args);  // /unpin

    // Agent todo list (model-maintained via the update_tasks tool; /tasks shows it).
    struct Task { std::string title; std::string status; };
    std::vector<Task> _tasks;
    std::string set_tasks(const JSON& tasks);   // update_tasks handler
    std::string tasks_command() const;          // /tasks view
    std::string shell_passthrough(const std::string& cmd); // !cmd: run locally, record for the model

    // Session change tracking for write_file (transparency + revert).
    struct FileChange { bool existed = false; std::string original; bool tracked = true; };
    std::map<std::string, FileChange> _changes; // absolute path -> pre-write state
    void record_file_change(const std::string& tool, const JSON& args); // pre-run hook
    std::string changes_command(const std::string& args);
    std::string export_transcript(const std::string& path);
    // Summarise the OLD part of the conversation via one LLM call, keeping the
    // last `keep_tail` user exchanges verbatim (0 = summarise everything). The
    // session's tasks/changes are carried into the summary verbatim.
    std::string compact_history(size_t keep_tail = 2);

    // A sink for slow-command progress text (wired to the REPL's status line), so
    // /compact can show a live progress bar while it summarises.
    std::function<void(const std::string&)> _progress_cb;
    // One compact transcript line per executed tool call (e.g. "⚙ read_file
    // src/x.cpp · 0.3s"). May be called from worker/pool threads.
    std::function<void(const std::string&)> _tool_notice_cb;
public:
    void set_progress_callback(std::function<void(const std::string&)> cb) { _progress_cb = std::move(cb); }
    void set_tool_notice_callback(std::function<void(const std::string&)> cb) { _tool_notice_cb = std::move(cb); }
private:
    // Switch the active provider mid-session, carrying the current conversation
    // over (re-auth non-interactively). Returns text to display.
    std::string switch_provider(const std::string& name);

    // Advisor: consult a stronger model (one-shot, non-streaming) and return its
    // advice; register/unregister the consult_advisor tool to match the config.
    std::string ask_advisor(const std::string& question);
    void sync_advisor_tool();

    // Workflows (claude only): register/unregister the run_workflow tool, render
    // the /workflows view, and fold finished runs' results into the conversation.
    void sync_workflow_tool();
    std::string workflows_command(const std::string& args);
    void deliver_workflow_results();

    // Register/unregister the web_search tool to match the config.
    void sync_web_search_tool();

    // Connect configured MCP servers and register their tools; render /mcp.
    void connect_mcp();
    void register_mcp_tools(); // (re)register proxy tools + resource readers
    std::string mcp_command(const std::string& args);
    std::vector<std::string> _mcp_tool_names; // currently-registered mcp__ tools

    Config _config;
    api::Client _client;
    tools::Registry _registry;
    std::unique_ptr<providers::Provider> _provider;
    Conversation _conversation;
    TokenStats _stats;
    WorkflowManager _workflows;
    mcp::Client _mcp;
};

} // namespace agent
