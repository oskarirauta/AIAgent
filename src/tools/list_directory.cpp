#include "agent/tools/list_directory.hpp"

#include <filesystem>
#include <sstream>
#include <vector>
#include <algorithm>
#include "common.hpp"

namespace agent::tools {

namespace { constexpr size_t MAX_ENTRIES = 1000; }

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
    namespace fs = std::filesystem;
    std::string path = ".";
    if ( args.contains("path"))
        path = common::trim_ws(args["path"].to_string());

    std::error_code ec;
    if ( !fs::exists(path, ec))
        return std::string("error: path does not exist: ") + path;
    if ( !fs::is_directory(path, ec))
        return std::string("error: not a directory: ") + path;

    // Collect (is_dir, name), then sort dirs-first and alphabetically — the other
    // tools all give bounded, ordered output; match that here.
    std::vector<std::pair<bool, std::string>> entries;
    for ( const auto& entry : fs::directory_iterator(path, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code de;
        entries.emplace_back(entry.is_directory(de), entry.path().filename().string());
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        if ( a.first != b.first ) return a.first > b.first;   // directories first
        return a.second < b.second;
    });

    std::ostringstream ss;
    size_t shown = 0;
    for ( const auto& e : entries ) {
        if ( shown++ >= MAX_ENTRIES ) {
            ss << "… (" << ( entries.size() - MAX_ENTRIES ) << " more entries; narrow the path)\n";
            break;
        }
        ss << ( e.first ? "[dir]  " : "[file] " ) << e.second << "\n";
    }

    return ss.str();
}

} // namespace agent::tools
