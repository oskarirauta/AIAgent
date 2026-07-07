#include "agent/tools/write_file.hpp"

#include <fstream>
#include "common.hpp"

namespace agent::tools {

JSON WriteFile::parameters() const {
    return JSON::Object{
        { "type", "object" },
        { "properties", JSON::Object{
            { "path", JSON::Object{
                { "type", "string" },
                { "description", "absolute or relative path to the file" }
            }},
            { "content", JSON::Object{
                { "type", "string" },
                { "description", "content to write" }
            }}
        }},
        { "required", JSON::Array{ "path", "content" }}
    };
}

std::string WriteFile::execute(const JSON& args) {
    std::string path = common::trim_ws(args["path"].to_string());
    std::string content = args["content"].to_string();

    // Lost-update guard: refuse to overwrite a file that changed on disk since the
    // model last read it, so an edit based on a stale copy can't silently clobber.
    if ( _tracker ) {
        std::string stale = _tracker->stale_reason(path);
        if ( !stale.empty())
            return "error: " + stale;
    }

    std::ofstream ofd(path, std::ios::out | std::ios::trunc);
    if ( !ofd.is_open())
        return std::string("error: cannot open file for writing: ") + path;

    ofd << content;
    ofd.close();

    if ( !ofd.good())
        return std::string("error: failed to write file: ") + path;

    if ( _tracker )
        _tracker->note(path); // stamp the new version so the next write compares to it
    return std::string("ok: wrote ") + std::to_string(content.size()) + " bytes";
}

} // namespace agent::tools
