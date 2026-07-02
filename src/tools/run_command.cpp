#include "run_command.hpp"

#include <array>
#include <cstdio>
#include <memory>
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

    cmd += " 2>&1"; // merge stderr to stdout

    std::array<char, 4096> buffer;
    std::string result;

    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if ( !pipe )
        return "error: failed to run command";

    while ( fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr )
        result += buffer.data();

    return result;
}

} // namespace agent::tools
