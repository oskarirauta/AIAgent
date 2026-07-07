#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent/config.hpp"

namespace agent {

// One step of a workflow run: a self-contained task handed to a sub-agent.
struct WorkflowStep {
    std::string task;
    std::string status = "pending"; // pending | running | done | error
    std::string result;
};

// A workflow run: a named sequence of steps executed on a background thread,
// independent of the interactive session that launched it.
struct WorkflowRun {
    int id = 0;
    std::string name;
    std::string status = "running"; // running | done | error | cancelled
    std::vector<WorkflowStep> steps;
    bool parallel = false; // steps run concurrently (independent tasks)
    long started_at = 0;  // epoch seconds
    long finished_at = 0; // epoch seconds, 0 while running
    bool delivered = false; // results already folded into the conversation
};

// Runs a single workflow step (task) to completion and returns its final text.
// Self-contained: it spins up its own client/provider/conversation so it is safe
// to call from a background thread. Declared here, defined in workflow.cpp.
std::string run_workflow_step(const Config& cfg, const std::string& task,
                              std::atomic<bool>* abort);

// Tracks and drives background workflow runs. Thread-safe.
class WorkflowManager {
public:
    // Executes one step's task; supplied by the launcher so the manager stays
    // decoupled from provider details. Runs on a background thread.
    using runner_fn = std::function<std::string(const std::string& task, std::atomic<bool>* abort)>;

    ~WorkflowManager();

    // Launch a run in the background; returns the new run id. With parallel=true
    // the steps run concurrently (bounded pool) — for independent tasks only.
    int launch(const std::string& name, const std::vector<std::string>& steps, runner_fn runner,
               bool parallel = false);

    // Thread-safe snapshot of all runs (newest last).
    std::vector<WorkflowRun> snapshot() const;
    bool any_running() const;

    // Finished runs not yet folded into the conversation; marks them delivered.
    std::vector<WorkflowRun> take_undelivered();

    // Cancel one running run (its in-flight step is aborted via the runner's
    // abort flag). Returns false if the id is unknown or already finished.
    bool cancel(int id);

    // Relaunch a finished run under a new id, keeping (and skipping) the steps
    // that already succeeded — only failed/pending/cancelled steps run again.
    // Returns the new id, or -1 (unknown / still running / nothing to retry).
    int retry(int id, runner_fn runner);

    // Called from the run's thread when it finishes (any final status). Used for
    // completion notifications; invoked with a snapshot of the run. Cleared with
    // nullptr; setting blocks while a callback is in flight, so after
    // set_on_finish(nullptr) returns no further calls are made.
    void set_on_finish(std::function<void(const WorkflowRun&)> cb);

    // Signal every run to abort and join their threads. Safe to call twice.
    void shutdown();

private:
    struct Entry {
        WorkflowRun run;      // guarded by _mx
        std::thread thread;
        std::atomic<bool> abort{ false }; // per-run cancel flag
    };
    void run_entry(Entry* e, runner_fn runner); // thread body (serial or parallel)

    mutable std::mutex _mx;
    std::vector<std::unique_ptr<Entry>> _entries; // stable Entry* for the threads
    std::atomic<int> _next_id{ 1 };
    std::atomic<bool> _abort{ false };

    std::mutex _cb_mx; // guards _on_finish, held during invocation
    std::function<void(const WorkflowRun&)> _on_finish;
};

} // namespace agent
