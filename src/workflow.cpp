#include "agent/workflow.hpp"

#include <ctime>
#include "json.hpp"
#include "agent/api/client.hpp"
#include "agent/conversation.hpp"
#include "agent/providers/provider.hpp"
#include "agent/tools/registry.hpp"
#include "agent/tools/read_file.hpp"
#include "agent/tools/list_directory.hpp"
#include "agent/tools/grep.hpp"
#include "agent/text_utils.hpp"

namespace agent {

static long now_seconds() {
    return static_cast<long>(std::time(nullptr));
}

// A headless agent loop (no streaming, no UI): send the conversation, run any
// tool calls, loop until the model answers or a cap is hit. Kept separate from
// Repl::process_turn so a sub-agent can run with its own provider/client/tools.
static std::string headless_loop(providers::Provider& provider, api::Client& client,
                                 tools::Registry& registry, Conversation& conv,
                                 std::atomic<bool>* abort) {
    const int max_iterations = 24; // safety cap against a runaway tool loop
    for ( int iter = 0; iter < max_iterations; ++iter ) {
        if ( abort && abort->load(std::memory_order_relaxed))
            return "cancelled";

        JSON tools = registry.schema();
        JSON request = provider.build_request(conv, tools);
        std::string body = request.dump_minified();

        // No prepare_request(): a sub-agent reuses the already-valid token loaded
        // at construction, so it never triggers an interactive re-login.
        std::string resp_str = client.post(provider.endpoint(), provider.auth_header(),
                                            provider.auth_value(), provider.extra_headers(),
                                            body, abort);
        if ( abort && abort->load(std::memory_order_relaxed))
            return "cancelled";
        if ( resp_str.empty())
            return "error: empty response from provider";

        providers::Response resp;
        try {
            resp = provider.parse_response(JSON::parse(resp_str));
        } catch ( const std::exception& e ) {
            return std::string("error: ") + e.what();
        }
        if ( !resp.success )
            return "error: " + resp.message;

        std::string text = agent::normalize_text(resp.message);

        std::vector<agent::ToolCall> calls;
        for ( const auto& tc : resp.tool_calls )
            calls.push_back({ tc.id, tc.name, tc.arguments.dump_minified() });
        if ( !text.empty() || !calls.empty())
            conv.add_assistant(text, calls);

        if ( resp.tool_calls.empty())
            return text.empty() ? "(no output)" : text;

        for ( const auto& tc : resp.tool_calls ) {
            std::string result;
            try {
                result = registry.execute(tc.name, tc.arguments);
            } catch ( const std::exception& e ) {
                result = std::string("error: ") + e.what();
            }
            conv.add_tool_result(tc.id, tc.name, result);
        }
    }
    return "error: workflow step exceeded the tool-iteration limit";
}

std::string run_workflow_step(const Config& cfg, const std::string& task,
                              std::atomic<bool>* abort) {
    api::Client client;
    auto provider = providers::create(cfg);
    if ( !provider )
        return "error: could not create provider";
    if ( !cfg.thinking.empty())
        provider->apply_provider_options(JSON::Object{ { "thinking", cfg.thinking } });

    // Sub-agents get read-only tools only and run them without confirmation:
    // no writes or shell execution happen unattended in the background.
    tools::Registry registry;
    registry.add(std::make_unique<tools::ReadFile>());
    registry.add(std::make_unique<tools::ListDirectory>());
    registry.add(std::make_unique<tools::Grep>());
    registry.set_mode(tools::ConfirmMode::automatic);

    Conversation conv;
    conv.set_system(
        "You are a focused sub-agent running one step of a larger workflow. Complete "
        "the assigned task using the read-only tools available (reading files, listing "
        "directories, grep). Do not ask questions — work from what you can inspect. "
        "Return a concise, self-contained result: what you found or concluded, with the "
        "key file paths and facts the main agent will need.");
    conv.add_user(task);

    return headless_loop(*provider, client, registry, conv, abort);
}

// ── WorkflowManager ──────────────────────────────────────────────────────

WorkflowManager::~WorkflowManager() {
    shutdown();
}

int WorkflowManager::launch(const std::string& name, const std::vector<std::string>& steps,
                            runner_fn runner) {
    auto entry = std::make_unique<Entry>();
    int id = _next_id.fetch_add(1);
    entry->run.id = id;
    entry->run.name = name.empty() ? ("workflow-" + std::to_string(id)) : name;
    entry->run.started_at = now_seconds();
    for ( const auto& s : steps )
        entry->run.steps.push_back(WorkflowStep{ s });

    Entry* e = entry.get(); // heap-stable; the vector only moves the unique_ptr
    {
        std::lock_guard<std::mutex> lk(_mx);
        _entries.push_back(std::move(entry));
    }

    e->thread = std::thread([this, e, runner]() {
        bool had_error = false;
        size_t count;
        {
            std::lock_guard<std::mutex> lk(_mx);
            count = e->run.steps.size();
        }
        for ( size_t i = 0; i < count; ++i ) {
            if ( _abort.load(std::memory_order_relaxed))
                break;
            std::string task;
            {
                std::lock_guard<std::mutex> lk(_mx);
                e->run.steps[i].status = "running";
                task = e->run.steps[i].task;
            }
            std::string result;
            bool step_error = false;
            try {
                result = runner(task, &_abort);
            } catch ( const std::exception& ex ) {
                result = std::string("error: ") + ex.what();
                step_error = true;
            }
            if ( result.rfind("error:", 0) == 0 )
                step_error = true;
            {
                std::lock_guard<std::mutex> lk(_mx);
                e->run.steps[i].result = result;
                e->run.steps[i].status = step_error ? "error" : "done";
            }
            if ( step_error ) { had_error = true; break; }
        }
        std::lock_guard<std::mutex> lk(_mx);
        e->run.finished_at = now_seconds();
        e->run.status = _abort.load(std::memory_order_relaxed) ? "cancelled"
                      : ( had_error ? "error" : "done" );
    });

    return id;
}

std::vector<WorkflowRun> WorkflowManager::snapshot() const {
    std::lock_guard<std::mutex> lk(_mx);
    std::vector<WorkflowRun> out;
    out.reserve(_entries.size());
    for ( const auto& e : _entries )
        out.push_back(e->run);
    return out;
}

bool WorkflowManager::any_running() const {
    std::lock_guard<std::mutex> lk(_mx);
    for ( const auto& e : _entries )
        if ( e->run.status == "running" )
            return true;
    return false;
}

std::vector<WorkflowRun> WorkflowManager::take_undelivered() {
    std::lock_guard<std::mutex> lk(_mx);
    std::vector<WorkflowRun> out;
    for ( auto& e : _entries ) {
        if ( e->run.status != "running" && !e->run.delivered ) {
            e->run.delivered = true;
            out.push_back(e->run);
        }
    }
    return out;
}

void WorkflowManager::shutdown() {
    _abort.store(true, std::memory_order_relaxed);
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(_mx);
        for ( auto& e : _entries )
            if ( e->thread.joinable())
                threads.push_back(std::move(e->thread));
    }
    for ( auto& t : threads )
        if ( t.joinable())
            t.join();
}

} // namespace agent
