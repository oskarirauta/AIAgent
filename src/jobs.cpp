#include "agent/jobs.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

namespace agent {

namespace {
// Keep at most this many bytes of a job's output (the tail), so a chatty server
// can run for hours without unbounded memory growth.
constexpr size_t OUTPUT_CAP = 256 * 1024;

// Shell-quote a value for a NAME=value export (single-quoted, ' escaped).
std::string sh_quote(const std::string& v) {
    std::string out = "'";
    for ( char c : v ) {
        if ( c == '\'' ) out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}
} // namespace

BackgroundJobs::~BackgroundJobs() {
    stop_all();
    // Join every reader thread so none outlives this object.
    std::vector<std::unique_ptr<BackgroundJob>> jobs;
    {
        std::lock_guard<std::mutex> lk(_mx);
        jobs.swap(_jobs);
    }
    for ( auto& j : jobs )
        if ( j->reader.joinable())
            j->reader.join();
}

int BackgroundJobs::start(const std::string& command, const std::string& cwd,
                          const std::vector<std::string>& extra_env, std::string& error) {
    // Prepend `export NAME=value;` for each entry, like run_command does, so the
    // job sees the same environment without polluting our own process.
    std::string exports;
    for ( const auto& e : extra_env ) {
        auto eq = e.find('=');
        if ( eq == std::string::npos ) continue;
        exports += "export " + e.substr(0, eq) + "=" + sh_quote(e.substr(eq + 1)) + "; ";
    }
    std::string shell_cmd = exports + command;

    int fds[2];
    if ( pipe(fds) != 0 ) {
        error = std::string("pipe: ") + std::strerror(errno);
        return -1;
    }

    pid_t pid = fork();
    if ( pid < 0 ) {
        error = std::string("fork: ") + std::strerror(errno);
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    if ( pid == 0 ) {
        // Child: own process group (so a stop kills the whole tree), stdout+stderr
        // to the pipe, then exec the shell. Nothing here may throw.
        setpgid(0, 0);
        if ( !cwd.empty()) { if ( chdir(cwd.c_str()) != 0 ) _exit(126); }
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        // stdin from /dev/null so an interactive command can't steal the terminal.
        int devnull = open("/dev/null", O_RDONLY);
        if ( devnull >= 0 ) { dup2(devnull, STDIN_FILENO); close(devnull); }
        execl("/bin/sh", "sh", "-c", shell_cmd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    // Parent.
    close(fds[1]);
    setpgid(pid, pid); // race-free: set here too, in case the child hasn't yet

    auto job = std::make_unique<BackgroundJob>();
    int id;
    BackgroundJob* jp;
    {
        std::lock_guard<std::mutex> lk(_mx);
        reap_done();
        id = _next_id++;
        job->id = id;
        job->command = command;
        job->pid = pid;
        job->pgid = pid;
        job->start = std::chrono::steady_clock::now();
        jp = job.get();
        _jobs.push_back(std::move(job));
    }

    int rfd = fds[0];
    jp->reader = std::thread([this, jp, rfd]() {
        char buf[8192];
        for (;;) {
            ssize_t n = read(rfd, buf, sizeof(buf));
            if ( n > 0 ) {
                std::lock_guard<std::mutex> lk(jp->mx);
                jp->output.append(buf, static_cast<size_t>(n));
                // Compact only when we drift well past the cap, so a chatty job does
                // not pay an O(cap) memmove on every read — trim back to the tail.
                if ( jp->output.size() > OUTPUT_CAP * 2 )
                    jp->output.erase(0, jp->output.size() - OUTPUT_CAP);
                continue;
            }
            if ( n < 0 && errno == EINTR )
                continue; // a signal (SIGWINCH/SIGINT) interrupted the read, not EOF
            break;        // EOF (0) or a genuine error
        }
        close(rfd);
        int status = 0;
        while ( waitpid(jp->pid, &status, 0) < 0 && errno == EINTR ) {}
        jp->exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
                                          : ( WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1 );
        jp->ended = std::chrono::steady_clock::now();
        jp->running.store(false, std::memory_order_release); // publishes exit_code/ended
        // Invoke the completion callback under _cb_mx so a concurrent
        // set_on_finish(nullptr) at teardown cannot race with (or free out from
        // under) an in-flight callback.
        std::lock_guard<std::mutex> lk(_cb_mx);
        if ( _on_finish ) {
            std::string st = jp->killed.load() ? "stopped"
                            : ( jp->exit_code == 0 ? "exited ok"
                                                   : "exited " + std::to_string(jp->exit_code));
            _on_finish(jp->id, jp->command, st);
        }
    });
    return id;
}

std::vector<JobInfo> BackgroundJobs::list() {
    std::lock_guard<std::mutex> lk(_mx);
    reap_done();
    std::vector<JobInfo> out;
    for ( const auto& j : _jobs ) {
        bool run = j->running.load(std::memory_order_acquire);
        auto end = run ? std::chrono::steady_clock::now() : j->ended;
        long secs = std::chrono::duration_cast<std::chrono::seconds>(end - j->start).count();
        size_t olen;
        { std::lock_guard<std::mutex> ol(j->mx); olen = j->output.size(); }
        out.push_back({ j->id, j->command, run, j->exit_code, j->killed.load(), secs, olen });
    }
    return out;
}

std::string BackgroundJobs::output(int id, size_t tail_lines, bool& found) {
    std::lock_guard<std::mutex> lk(_mx);
    found = false;
    for ( const auto& j : _jobs ) {
        if ( j->id != id ) continue;
        found = true;
        std::string full;
        { std::lock_guard<std::mutex> ol(j->mx); full = j->output; }
        if ( tail_lines == 0 )
            return full;
        // Keep only the last `tail_lines` lines.
        size_t pos = full.size();
        size_t lines = 0;
        while ( pos > 0 ) {
            size_t nl = full.rfind('\n', pos - 1);
            if ( nl == std::string::npos ) { pos = 0; break; }
            if ( ++lines >= tail_lines + 1 ) { pos = nl + 1; break; }
            pos = nl;
        }
        return full.substr(pos);
    }
    return "";
}

bool BackgroundJobs::stop(int id) {
    std::lock_guard<std::mutex> lk(_mx);
    for ( const auto& j : _jobs ) {
        if ( j->id != id ) continue;
        if ( j->running.load(std::memory_order_acquire) && j->pgid > 0 ) {
            j->killed.store(true);
            kill(-j->pgid, SIGTERM);
        }
        return true;
    }
    return false;
}

int BackgroundJobs::stop_all() {
    std::lock_guard<std::mutex> lk(_mx);
    int n = 0;
    for ( const auto& j : _jobs ) {
        if ( j->running.load(std::memory_order_acquire) && j->pgid > 0 ) {
            j->killed.store(true);
            kill(-j->pgid, SIGTERM);
            ++n;
        }
    }
    return n;
}

bool BackgroundJobs::empty() {
    std::lock_guard<std::mutex> lk(_mx);
    return _jobs.empty();
}

void BackgroundJobs::reap_done() {
    // Join reader threads of jobs that have finished, so they don't accumulate.
    // Called with _mx held. Finished jobs are kept in the list (for /jobs history)
    // but their thread is joined once.
    for ( auto& j : _jobs )
        if ( !j->running.load(std::memory_order_acquire) && j->reader.joinable())
            j->reader.join();
}

} // namespace agent
