#include "run_command.hpp"

#include <string>
#include "process.hpp"
#include "common.hpp"

namespace agent::tools {

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
        std::string out = proc[STREAM_OUT];
        std::string err = proc[STREAM_ERR];
        int code = proc[STREAM_STATUS];

        std::string result = out;
        if ( !err.empty()) {
            if ( !result.empty()) result += "\n";
            result += "stderr: " + err;
        }
        if ( code != 0 ) {
            if ( !result.empty()) result += "\n";
            result += "exit code: " + std::to_string(code);
        }
        return result;
    } catch ( const std::exception& e ) {
        return std::string("error: ") + e.what();
    }
}

} // namespace agent::tools
