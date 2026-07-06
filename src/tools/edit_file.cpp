#include "agent/tools/edit_file.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include "common.hpp"

namespace agent::tools {

namespace {

bool json_truthy(const JSON& v) {
    if ( v == JSON::TYPE::STRING ) {
        std::string s = common::to_lower(v.to_string());
        return s == "true" || s == "1" || s == "yes";
    }
    return v.to_bool();
}

} // namespace

JSON EditFile::parameters() const {
    return JSON::Object{
        { "type", "object" },
        { "properties", JSON::Object{
            { "path", JSON::Object{
                { "type", "string" },
                { "description", "absolute or relative path to the file to edit" }
            }},
            { "old_string", JSON::Object{
                { "type", "string" },
                { "description", "the exact text to replace (unique unless replace_all)" }
            }},
            { "new_string", JSON::Object{
                { "type", "string" },
                { "description", "the replacement text" }
            }},
            { "replace_all", JSON::Object{
                { "type", "boolean" },
                { "description", "replace every occurrence instead of requiring a unique match (optional)" }
            }}
        }},
        { "required", JSON::Array{ "path", "old_string", "new_string" }}
    };
}

std::string EditFile::execute(const JSON& args) {
    std::string path = common::trim_ws(args["path"].to_string());
    if ( path.empty())
        return "error: provide a file `path`";
    std::string old_s = args.contains("old_string") ? args["old_string"].to_string() : "";
    std::string new_s = args.contains("new_string") ? args["new_string"].to_string() : "";
    bool replace_all = args.contains("replace_all") && json_truthy(args["replace_all"]);

    if ( old_s.empty())
        return "error: `old_string` must be non-empty (use write_file to create a file)";
    if ( old_s == new_s )
        return "error: `old_string` and `new_string` are identical";

    std::error_code ec;
    if ( !std::filesystem::is_regular_file(path, ec))
        return "error: file does not exist: " + path + " (use write_file to create it)";

    std::string content;
    {
        std::ifstream ifd(path, std::ios::in | std::ios::binary);
        if ( !ifd.is_open())
            return "error: cannot read file: " + path;
        std::stringstream ss; ss << ifd.rdbuf();
        content = ss.str();
    }

    // Count matches.
    size_t count = 0, scan = 0;
    while (( scan = content.find(old_s, scan)) != std::string::npos ) { ++count; scan += old_s.size(); }
    if ( count == 0 )
        return "error: old_string not found in " + path + " (it must match exactly, including whitespace)";
    if ( count > 1 && !replace_all )
        return "error: old_string appears " + std::to_string(count) + " times in " + path +
               "; add surrounding context to make it unique, or set replace_all=true";

    std::string result;
    size_t replacements = 0;
    if ( replace_all ) {
        size_t pos = 0;
        while ( pos < content.size()) {
            size_t hit = content.find(old_s, pos);
            if ( hit == std::string::npos ) { result += content.substr(pos); break; }
            result += content.substr(pos, hit - pos) + new_s;
            pos = hit + old_s.size();
            ++replacements;
        }
    } else {
        size_t hit = content.find(old_s);
        result = content.substr(0, hit) + new_s + content.substr(hit + old_s.size());
        replacements = 1;
    }

    std::ofstream ofd(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if ( !ofd.is_open())
        return "error: cannot open file for writing: " + path;
    ofd << result;
    ofd.close();
    if ( !ofd.good())
        return "error: failed to write file: " + path;

    return "ok: edited " + path + " (" + std::to_string(replacements) +
           ( replacements == 1 ? " replacement)" : " replacements)" );
}

} // namespace agent::tools
