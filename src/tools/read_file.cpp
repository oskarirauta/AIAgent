#include "agent/tools/read_file.hpp"

#include <fstream>
#include <sstream>
#include "common.hpp"

namespace agent::tools {

JSON ReadFile::parameters() const {
    return JSON::Object{
        { "type", "object" },
        { "properties", JSON::Object{
            { "path", JSON::Object{
                { "type", "string" },
                { "description", "absolute or relative path to the file" }
            }}
        }},
        { "required", JSON::Array{ "path" }}
    };
}

std::string ReadFile::execute(const JSON& args) {
    std::string path = common::trim_ws(args["path"].to_string());

    std::ifstream ifd(path, std::ios::in);
    if ( !ifd.is_open())
        return std::string("error: cannot open file: ") + path;

    std::stringstream ss;
    ss << ifd.rdbuf();
    return ss.str();
}

} // namespace agent::tools
