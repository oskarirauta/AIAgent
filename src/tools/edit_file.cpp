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

struct EditResult {
    bool ok = false;
    std::string error;   // reason when !ok (a substring like "not found"/"identical"/"appears N times")
    std::string content; // updated content when ok
    size_t replacements = 0;
};

// Apply one old->new replacement to `content` (sequential — later edits see the
// result of earlier ones). Enforces a unique match unless replace_all.
EditResult apply_one(const std::string& content, const std::string& old_s,
                     const std::string& new_s, bool replace_all) {
    if ( old_s.empty())
        return { false, "old_string must be non-empty (use write_file to create a file)", {}, 0 };
    if ( old_s == new_s )
        return { false, "old_string and new_string are identical", {}, 0 };

    size_t count = 0, scan = 0;
    while (( scan = content.find(old_s, scan)) != std::string::npos ) { ++count; scan += old_s.size(); }
    if ( count == 0 )
        return { false, "old_string not found (it must match exactly, including whitespace)", {}, 0 };
    if ( count > 1 && !replace_all )
        return { false, "old_string appears " + std::to_string(count) +
                        " times; add surrounding context to make it unique, or set replace_all=true", {}, 0 };

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
    return { true, "", result, replacements };
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
            }},
            { "edits", JSON::Object{
                { "type", "array" },
                { "items", JSON::Object{
                    { "type", "object" },
                    { "properties", JSON::Object{
                        { "old_string", JSON::Object{ { "type", "string" } } },
                        { "new_string", JSON::Object{ { "type", "string" } } },
                        { "replace_all", JSON::Object{ { "type", "boolean" } } }
                    }},
                    { "required", JSON::Array{ "old_string", "new_string" } }
                }},
                { "description", "apply several edits to the file in one atomic call (instead of "
                                 "old_string/new_string). Edits apply in order — a later edit sees the "
                                 "result of earlier ones. If any edit fails, the file is left unchanged." }
            }}
        }},
        { "required", JSON::Array{ "path" }}
    };
}

std::string EditFile::execute(const JSON& args) {
    std::string path = common::trim_ws(args["path"].to_string());
    if ( path.empty())
        return "error: provide a file `path`";

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

    std::string result = content;
    size_t total_replacements = 0;
    size_t edit_count = 0;

    if ( args.contains("edits") && args["edits"] == JSON::TYPE::ARRAY ) {
        // Multi-edit: apply each edit in order to an in-memory copy; only write if
        // every edit succeeds (atomic — a failure leaves the file untouched).
        JSON edits = args["edits"];
        if ( edits.size() == 0 )
            return "error: `edits` is empty";
        for ( size_t i = 0; i < edits.size(); ++i ) {
            JSON e = edits[i];
            std::string old_s = e.contains("old_string") ? e["old_string"].to_string() : "";
            std::string new_s = e.contains("new_string") ? e["new_string"].to_string() : "";
            bool ra = e.contains("replace_all") && json_truthy(e["replace_all"]);
            EditResult r = apply_one(result, old_s, new_s, ra);
            if ( !r.ok )
                return "error: edit #" + std::to_string(i + 1) + ": " + r.error +
                       " (in " + path + "; file left unchanged)";
            result = r.content;
            total_replacements += r.replacements;
            ++edit_count;
        }
    } else {
        // Single edit (old_string / new_string).
        std::string old_s = args.contains("old_string") ? args["old_string"].to_string() : "";
        std::string new_s = args.contains("new_string") ? args["new_string"].to_string() : "";
        bool ra = args.contains("replace_all") && json_truthy(args["replace_all"]);
        EditResult r = apply_one(result, old_s, new_s, ra);
        if ( !r.ok )
            return "error: " + r.error + " (in " + path + ")";
        result = r.content;
        total_replacements = r.replacements;
        edit_count = 1;
    }

    std::ofstream ofd(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if ( !ofd.is_open())
        return "error: cannot open file for writing: " + path;
    ofd << result;
    ofd.close();
    if ( !ofd.good())
        return "error: failed to write file: " + path;

    if ( edit_count > 1 )
        return "ok: edited " + path + " (" + std::to_string(edit_count) + " edits, " +
               std::to_string(total_replacements) + " replacements)";
    return "ok: edited " + path + " (" + std::to_string(total_replacements) +
           ( total_replacements == 1 ? " replacement)" : " replacements)" );
}

} // namespace agent::tools
