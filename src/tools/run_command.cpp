#include "agent/tools/run_command.hpp"

#include <string>
#include "process.hpp"
#include "env.hpp"
#include "common.hpp"
#include "agent/signal_handler.hpp"

namespace agent::tools {

namespace {

// A runaway command must neither hang the agent nor flood the model's context.
// A user Ctrl-C (agent::turn_abort) also interrupts the command.
constexpr int    COMMAND_TIMEOUT_MS = 120000;
constexpr size_t MAX_OUTPUT_BYTES   = 100 * 1024;      // what the model receives
constexpr size_t CAPTURE_GUARD_BYTES = 2 * 1024 * 1024; // hard cap at the process
constexpr size_t HEAD_BYTES         = 16 * 1024;        // kept from the start
constexpr size_t TAIL_BYTES         = 80 * 1024;        // kept from the end

// Keep the head AND the tail of over-long output. Builds print thousands of
// warnings before the one error that matters, and test runners put their
// summary last — a keep-first-N cap loses exactly the part the model needs.
std::string head_tail_trim(const std::string& s) {
    if ( s.size() <= MAX_OUTPUT_BYTES )
        return s;
    std::string head = s.substr(0, HEAD_BYTES);
    size_t nl = head.rfind('\n');
    if ( nl != std::string::npos )
        head.resize(nl + 1);
    std::string tail = s.substr(s.size() - TAIL_BYTES);
    nl = tail.find('\n');
    if ( nl != std::string::npos )
        tail.erase(0, nl + 1);
    size_t omitted = s.size() - head.size() - tail.size();
    return head + "\n[... " + std::to_string(omitted / 1024) +
           " KB omitted — head and tail of the output kept ...]\n\n" + tail;
}

} // namespace

JSON RunCommand::parameters() const {
    return JSON::Object{
        { "type", "object" },
        { "properties", JSON::Object{
            { "command", JSON::Object{
                { "type", "string" },
                { "description", "shell command to execute" }
            }},
            { "timeout", JSON::Object{
                { "type", "integer" },
                { "description", "max seconds to run before the command is killed (default 120, max 600)" }
            }},
            { "cwd", JSON::Object{
                { "type", "string" },
                { "description", "working directory to run the command in (optional)" }
            }},
            { "env", JSON::Object{
                { "type", "object" },
                { "description", "extra environment variables for this command only (name -> value)" }
            }}
        }},
        { "required", JSON::Array{ "command" }}
    };
}

// POSIX single-quote a string so it is safe to embed in a `cd <dir>` prefix.
static std::string shell_quote(const std::string& s) {
    std::string o = "'";
    for ( char c : s ) {
        if ( c == '\'' ) o += "'\\''";
        else o += c;
    }
    o += "'";
    return o;
}

std::string RunCommand::execute(const JSON& args) {
    std::string cmd = common::trim_ws(args["command"].to_string());
    if ( cmd.empty())
        return "error: empty command";

    // Optional timeout (seconds), clamped to a sane ceiling.
    int timeout_ms = COMMAND_TIMEOUT_MS;
    if ( args.contains("timeout")) {
        long secs = 0;
        if ( args["timeout"] == JSON::TYPE::INT ) secs = static_cast<long>(static_cast<long long>(args["timeout"]));
        else if ( args["timeout"] == JSON::TYPE::FLOAT ) secs = static_cast<long>(static_cast<double>(args["timeout"]));
        if ( secs > 0 ) {
            if ( secs > 600 ) secs = 600;
            timeout_ms = static_cast<int>(secs * 1000);
        }
    }

    // Optional working directory: run inside `cd <dir> && ( ... )`.
    std::string shell_cmd = cmd;
    if ( args.contains("cwd") && args["cwd"] == JSON::TYPE::STRING ) {
        std::string cwd = common::trim_ws(args["cwd"].to_string());
        if ( !cwd.empty())
            shell_cmd = "cd " + shell_quote(cwd) + " && ( " + cmd + " )";
    }

    // Optional per-call environment. Inject `export NAME=value` into the child
    // shell rather than mutating the global process environment with setenv:
    // setenv is not thread-safe, and run_command executes on the worker thread
    // while background workflow threads may be forking/reading the environment.
    if ( args.contains("env") && args["env"] == JSON::TYPE::OBJECT ) {
        auto valid_name = [](const std::string& n) {
            if ( n.empty() || std::isdigit(static_cast<unsigned char>(n[0]))) return false;
            for ( char ch : n )
                if ( !( std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' ))
                    return false;
            return true;
        };
        std::string exports;
        args["env"].for_each([&](JSON::fe_iterator& it, const JSON& v) {
            if ( it.is_object() && valid_name(it.name()))
                exports += "export " + it.name() + "=" + shell_quote(v.to_string()) + "; ";
        });
        if ( !exports.empty())
            shell_cmd = exports + shell_cmd;
    }

    try {
        process_t proc("/bin/sh", { "-c", shell_cmd });
        proc.timeout(timeout_ms)
            .abort_with(&agent::turn_abort)
            .max_output(CAPTURE_GUARD_BYTES); // trimmed head+tail below

        std::string out = proc[STREAM_OUT];   // first read drains under the bounds above
        std::string err = proc[STREAM_ERR];
        int code = proc[STREAM_STATUS];
        bool aborted = proc.aborted();
        bool timed_out = proc.timed_out();
        bool truncated = proc.truncated();

        std::string result = out;
        if ( !err.empty()) {
            if ( !result.empty()) result += "\n";
            result += "stderr: " + err;
        }
        result = head_tail_trim(result);

        if ( aborted ) {
            if ( !result.empty()) result += "\n";
            result += "(command interrupted and killed)";
        } else if ( timed_out ) {
            if ( !result.empty()) result += "\n";
            result += "(command timed out after " + std::to_string(timeout_ms / 1000) + "s and was killed)";
        } else if ( code != 0 ) {
            if ( !result.empty()) result += "\n";
            result += "exit code: " + std::to_string(code);
        }

        if ( truncated ) {
            if ( !result.empty()) result += "\n";
            result += "(output exceeded the " + std::to_string(CAPTURE_GUARD_BYTES / ( 1024 * 1024 )) +
                      " MB capture guard; the very middle may be missing beyond the trim above)";
        }

        // A bare empty string is ambiguous to the model — report the outcome.
        if ( result.empty()) {
            result = ( code == 0 )
                ? "(command completed successfully, exit code 0, no output)"
                : "(exit code " + std::to_string(code) + ", no output)";
        }
        return result;
    } catch ( const std::exception& e ) {
        return std::string("error: ") + e.what();
    }
}

} // namespace agent::tools
