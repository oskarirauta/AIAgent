#include "agent/tools/run_command.hpp"

#include <string>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <cerrno>
#include <ctime>
#include "common.hpp"
#include "agent/signal_handler.hpp"

namespace agent::tools {

namespace {

// Hard limits so a runaway command can neither hang the agent nor flood the
// model's context. A user Ctrl-C (agent::turn_abort) also interrupts the command.
constexpr int    COMMAND_TIMEOUT_SECS = 120;
constexpr size_t MAX_OUTPUT_BYTES     = 100 * 1024;

struct CmdResult {
    std::string out;
    std::string err;
    int  code = -1;
    bool timed_out = false;
    bool aborted = false;
    bool truncated = false;
    bool spawn_error = false;
    std::string spawn_message;
};

CmdResult run_shell(const std::string& cmd) {
    CmdResult r;

    int op[2], ep[2];
    if ( pipe(op) != 0 || pipe(ep) != 0 ) {
        r.spawn_error = true;
        r.spawn_message = "pipe() failed";
        return r;
    }

    pid_t pid = fork();
    if ( pid < 0 ) {
        r.spawn_error = true;
        r.spawn_message = "fork() failed";
        close(op[0]); close(op[1]); close(ep[0]); close(ep[1]);
        return r;
    }

    if ( pid == 0 ) {
        // Child: run in its own process group so a timeout/abort can kill the
        // whole process tree (the shell and anything it spawned).
        setpgid(0, 0);
        dup2(op[1], STDOUT_FILENO);
        dup2(ep[1], STDERR_FILENO);
        close(op[0]); close(op[1]); close(ep[0]); close(ep[1]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    // Parent.
    setpgid(pid, pid); // set from both sides to avoid the exec race
    close(op[1]); close(ep[1]);
    fcntl(op[0], F_SETFL, O_NONBLOCK);
    fcntl(ep[0], F_SETFL, O_NONBLOCK);

    time_t start = time(nullptr);
    bool out_open = true, err_open = true;
    bool killed = false;
    time_t kill_time = 0;
    size_t total = 0;

    while ( out_open || err_open ) {
        if ( !killed ) {
            if ( agent::turn_abort.load(std::memory_order_relaxed)) {
                r.aborted = true;
                killpg(pid, SIGTERM);
                killed = true;
                kill_time = time(nullptr);
            } else if ( time(nullptr) - start >= COMMAND_TIMEOUT_SECS ) {
                r.timed_out = true;
                killpg(pid, SIGTERM);
                killed = true;
                kill_time = time(nullptr);
            }
        } else if ( time(nullptr) - kill_time >= 2 ) {
            killpg(pid, SIGKILL); // escalate if it ignored SIGTERM
        }

        fd_set fds;
        FD_ZERO(&fds);
        int maxfd = -1;
        if ( out_open ) { FD_SET(op[0], &fds); maxfd = std::max(maxfd, op[0]); }
        if ( err_open ) { FD_SET(ep[0], &fds); maxfd = std::max(maxfd, ep[0]); }
        if ( maxfd < 0 ) break;

        struct timeval tv { 0, 200 * 1000 }; // 200 ms — bounds abort/timeout latency
        int sel = select(maxfd + 1, &fds, nullptr, nullptr, &tv);
        if ( sel < 0 ) {
            if ( errno == EINTR ) continue;
            break;
        }

        char buf[4096];
        auto drain = [&](int fd, bool& open, std::string& dst) {
            if ( !open || !FD_ISSET(fd, &fds)) return;
            ssize_t n = read(fd, buf, sizeof(buf));
            if ( n > 0 ) {
                if ( total < MAX_OUTPUT_BYTES ) {
                    size_t take = std::min(MAX_OUTPUT_BYTES - total, static_cast<size_t>(n));
                    dst.append(buf, take);
                    total += take;
                    if ( static_cast<size_t>(n) > take ) r.truncated = true;
                } else {
                    r.truncated = true; // past the cap: keep draining but discard
                }
            } else if ( n == 0 ) {
                open = false; // EOF
            }
            // n < 0 (EAGAIN): nothing available right now
        };
        drain(op[0], out_open, r.out);
        drain(ep[0], err_open, r.err);
    }

    close(op[0]); close(ep[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if ( WIFEXITED(status)) r.code = WEXITSTATUS(status);
    else if ( WIFSIGNALED(status)) r.code = 128 + WTERMSIG(status);
    return r;
}

} // namespace

JSON RunCommand::parameters() const {
    return JSON::Object{
        { "type", "object" },
        { "properties", JSON::Object{
            { "command", JSON::Object{
                { "type", "string" },
                { "description", "shell command to execute" }
            }}
        }},
        { "required", JSON::Array{ "command" }}
    };
}

std::string RunCommand::execute(const JSON& args) {
    std::string cmd = common::trim_ws(args["command"].to_string());
    if ( cmd.empty())
        return "error: empty command";

    CmdResult r = run_shell(cmd);
    if ( r.spawn_error )
        return "error: " + r.spawn_message;

    std::string result = r.out;
    if ( !r.err.empty()) {
        if ( !result.empty()) result += "\n";
        result += "stderr: " + r.err;
    }

    if ( r.aborted ) {
        if ( !result.empty()) result += "\n";
        result += "(command interrupted and killed)";
    } else if ( r.timed_out ) {
        if ( !result.empty()) result += "\n";
        result += "(command timed out after " + std::to_string(COMMAND_TIMEOUT_SECS) + "s and was killed)";
    } else if ( r.code != 0 ) {
        if ( !result.empty()) result += "\n";
        result += "exit code: " + std::to_string(r.code);
    }

    if ( r.truncated ) {
        if ( !result.empty()) result += "\n";
        result += "(output truncated at " + std::to_string(MAX_OUTPUT_BYTES / 1024) + " KB)";
    }

    // A bare empty string is ambiguous to the model — report the outcome.
    if ( result.empty()) {
        result = ( r.code == 0 )
            ? "(command completed successfully, exit code 0, no output)"
            : "(exit code " + std::to_string(r.code) + ", no output)";
    }
    return result;
}

} // namespace agent::tools
