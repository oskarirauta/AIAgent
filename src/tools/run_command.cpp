#include "agent/tools/run_command.hpp"

#include <string>
#include "process.hpp"
#include "common.hpp"
#include "agent/signal_handler.hpp"

namespace agent::tools {

namespace {

// A runaway command must neither hang the agent nor flood the model's context.
// A user Ctrl-C (agent::turn_abort) also interrupts the command.
constexpr int    COMMAND_TIMEOUT_MS = 120000;
constexpr size_t MAX_OUTPUT_BYTES   = 100 * 1024;

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

    try {
        process_t proc("/bin/sh", { "-c", cmd });
        proc.timeout(COMMAND_TIMEOUT_MS)
            .abort_with(&agent::turn_abort)
            .max_output(MAX_OUTPUT_BYTES);

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

        if ( aborted ) {
            if ( !result.empty()) result += "\n";
            result += "(command interrupted and killed)";
        } else if ( timed_out ) {
            if ( !result.empty()) result += "\n";
            result += "(command timed out after " + std::to_string(COMMAND_TIMEOUT_MS / 1000) + "s and was killed)";
        } else if ( code != 0 ) {
            if ( !result.empty()) result += "\n";
            result += "exit code: " + std::to_string(code);
        }

        if ( truncated ) {
            if ( !result.empty()) result += "\n";
            result += "(output truncated at " + std::to_string(MAX_OUTPUT_BYTES / 1024) + " KB)";
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
