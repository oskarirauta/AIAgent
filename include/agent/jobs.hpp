#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/types.h>

namespace agent {

// A long-running shell command started in the background so it does not block the
// turn — a dev server, a file watcher, `tail -f`. Output is captured live into a
// bounded buffer; the job can be inspected and stopped by id.
struct BackgroundJob {
    int id = 0;
    std::string command;
    pid_t pid = -1;
    pid_t pgid = -1;
    std::atomic<bool> running{ true };
    int exit_code = 0;
    bool killed = false;                 // stopped by us, not a natural exit
    std::chrono::steady_clock::time_point start;
    std::chrono::steady_clock::time_point ended;
    std::string output;                  // guarded by `mx`; capped to a tail
    std::mutex mx;
    std::thread reader;

    BackgroundJob() = default;
};

// Snapshot of a job's state for listing (no locks held by the caller).
struct JobInfo {
    int id;
    std::string command;
    bool running;
    int exit_code;
    bool killed;
    long seconds;      // elapsed (running) or total (finished)
    size_t output_len;
};

class BackgroundJobs {
public:
    ~BackgroundJobs();

    // Notice printed when a job exits (id, command, a short status). Set by the app.
    using finish_fn = std::function<void(int id, const std::string& command, const std::string& status)>;
    void set_on_finish(finish_fn fn) { _on_finish = std::move(fn); }

    // Start `command` under `/bin/sh -c` in `cwd`, with `extra_env` (NAME=value)
    // exported. Returns the new job id, or -1 on failure (message in `error`).
    int start(const std::string& command, const std::string& cwd,
              const std::vector<std::string>& extra_env, std::string& error);

    std::vector<JobInfo> list();
    // Captured output for a job (last `tail_lines` lines, 0 = all). Empty string
    // with `found=false` when there is no such job.
    std::string output(int id, size_t tail_lines, bool& found);
    bool stop(int id);       // SIGTERM the job's process group; false if unknown
    int stop_all();          // stop every running job; returns how many

    bool empty();

private:
    void reap_done();        // join reader threads of finished jobs (called under _mx)

    std::vector<std::unique_ptr<BackgroundJob>> _jobs;
    std::mutex _mx;
    int _next_id = 1;
    finish_fn _on_finish;
};

} // namespace agent
