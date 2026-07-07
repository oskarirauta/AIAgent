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
                            runner_fn runner, bool parallel) {
    auto entry = std::make_unique<Entry>();
    int id = _next_id.fetch_add(1);
    entry->run.id = id;
    entry->run.name = name.empty() ? ("workflow-" + std::to_string(id)) : name;
    entry->run.parallel = parallel;
    entry->run.started_at = now_seconds();
    for ( const auto& s : steps )
        entry->run.steps.push_back(WorkflowStep{ s });

    Entry* e = entry.get(); // heap-stable; the vector only moves the unique_ptr
    {
        std::lock_guard<std::mutex> lk(_mx);
        _entries.push_back(std::move(entry));
    }

    e->thread = std::thread([this, e, runner]() { run_entry(e, runner); });
    return id;
}

void WorkflowManager::run_entry(Entry* e, runner_fn runner) {
    bool parallel;
    size_t count;
    {
        std::lock_guard<std::mutex> lk(_mx);
        parallel = e->run.parallel;
        count = e->run.steps.size();
    }
    std::atomic<bool> had_error{ false };
    auto aborted = [this, e]() {
        return _abort.load(std::memory_order_relaxed) || e->abort.load(std::memory_order_relaxed);
    };

    // Runs one step (by index). Steps pre-marked done (a retry) are skipped.
    auto run_step = [&](size_t i) {
        std::string task;
        {
            std::lock_guard<std::mutex> lk(_mx);
            if ( e->run.steps[i].status == "done" )
                return;
            e->run.steps[i].status = "running";
            task = e->run.steps[i].task;
        }
        std::string result;
        bool step_error = false;
        try {
            result = runner(task, &e->abort);
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
        if ( step_error )
            had_error.store(true, std::memory_order_relaxed);
    };

    if ( parallel ) {
        // Independent steps: a small bounded pool. An error in one step does not
        // stop the others (unlike serial, where later steps likely depend on it).
        const size_t pool = std::min<size_t>(3, count);
        std::atomic<size_t> next{ 0 };
        std::vector<std::thread> workers;
        for ( size_t w = 0; w < pool; ++w )
            workers.emplace_back([&]() {
                for ( size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
                    if ( aborted())
                        return;
                    run_step(i);
                }
            });
        for ( auto& t : workers )
            t.join();
    } else {
        for ( size_t i = 0; i < count; ++i ) {
            if ( aborted())
                break;
            run_step(i);
            if ( had_error.load(std::memory_order_relaxed))
                break; // serial: later steps likely build on this one
        }
    }

    WorkflowRun finished;
    {
        std::lock_guard<std::mutex> lk(_mx);
        e->run.finished_at = now_seconds();
        e->run.status = aborted() ? "cancelled"
                      : ( had_error.load(std::memory_order_relaxed) ? "error" : "done" );
        finished = e->run;
    }
    // Completion notification. The lock is held during invocation so that
    // set_on_finish(nullptr) synchronises with in-flight callbacks.
    {
        std::lock_guard<std::mutex> lk(_cb_mx);
        if ( _on_finish )
            _on_finish(finished);
    }
}

bool WorkflowManager::cancel(int id) {
    std::lock_guard<std::mutex> lk(_mx);
    for ( auto& e : _entries ) {
        if ( e->run.id != id )
            continue;
        if ( e->run.status != "running" )
            return false;
        e->abort.store(true, std::memory_order_relaxed);
        return true;
    }
    return false;
}

int WorkflowManager::retry(int id, runner_fn runner) {
    // Copy the source run under the lock, then relaunch outside it.
    std::string name;
    std::vector<WorkflowStep> steps;
    bool parallel = false;
    {
        std::lock_guard<std::mutex> lk(_mx);
        bool found = false;
        for ( auto& e : _entries ) {
            if ( e->run.id != id )
                continue;
            if ( e->run.status == "running" )
                return -1;
            name = e->run.name;
            steps = e->run.steps;
            parallel = e->run.parallel;
            found = true;
            break;
        }
        if ( !found )
            return -1;
    }
    bool anything_left = false;
    for ( auto& s : steps ) {
        if ( s.status != "done" ) {
            s.status = "pending"; // error/cancelled/pending: run again
            s.result.clear();
            anything_left = true;
        }
    }
    if ( !anything_left )
        return -1; // every step already succeeded

    auto entry = std::make_unique<Entry>();
    int nid = _next_id.fetch_add(1);
    entry->run.id = nid;
    entry->run.name = name;
    entry->run.parallel = parallel;
    entry->run.started_at = now_seconds();
    entry->run.steps = std::move(steps); // done steps keep results; run_entry skips them

    Entry* e = entry.get();
    {
        std::lock_guard<std::mutex> lk(_mx);
        _entries.push_back(std::move(entry));
    }
    e->thread = std::thread([this, e, runner]() { run_entry(e, runner); });
    return nid;
}

void WorkflowManager::set_on_finish(std::function<void(const WorkflowRun&)> cb) {
    std::lock_guard<std::mutex> lk(_cb_mx);
    _on_finish = std::move(cb);
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
        for ( auto& e : _entries ) {
            // The runner only sees the per-run flag, so cancel each run too.
            e->abort.store(true, std::memory_order_relaxed);
            if ( e->thread.joinable())
                threads.push_back(std::move(e->thread));
        }
    }
    for ( auto& t : threads )
        if ( t.joinable())
            t.join();
}

} // namespace agent
