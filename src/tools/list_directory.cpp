#include "list_directory.hpp"

#include <filesystem>
#include <sstream>
#include "common.hpp"

namespace agent::tools {

JSON ListDirectory::parameters() const {
    return JSON::Object{
        { "type", "object" },
        { "properties", JSON::Object{
            { "path", JSON::Object{
                { "type", "string" },
                { "description", "directory path (default: current directory)" }
            }}
        }},
        { "required", JSON::Array{} }
    };
}

std::string ListDirectory::execute(const JSON& args) {
    std::string path = ".";
    if ( args.contains("path"))
        path = common::trim_ws(args["path"].to_string());

    if ( !std::filesystem::exists(path))
        return std::string("error: path does not exist: ") + path;

    std::ostringstream ss;
    for ( const auto& entry : std::filesystem::directory_iterator(path)) {
        ss << ( entry.is_directory() ? "[dir]  " : "[file] " )
           << entry.path().filename().string() << "\n";
    }

    return ss.str();
}

} // namespace agent::tools
