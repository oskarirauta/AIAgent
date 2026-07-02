#include "grep.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include "common.hpp"

namespace agent::tools {

JSON Grep::parameters() const {
    return JSON::Object{
        { "type", "object" },
        { "properties", JSON::Object{
            { "path", JSON::Object{
                { "type", "string" },
                { "description", "path to the file to search" }
            }},
            { "pattern", JSON::Object{
                { "type", "string" },
                { "description", "substring to search for" }
            }}
        }},
        { "required", JSON::Array{ "path", "pattern" }}
    };
}

std::string Grep::execute(const JSON& args) {
    std::string path = common::trimmed(args["path"].to_string(), common::whitespace);
    std::string pattern = common::trimmed(args["pattern"].to_string(), common::whitespace);

    if ( pattern.empty())
        return "error: empty pattern";

    std::ifstream ifd(path, std::ios::in);
    if ( !ifd.is_open())
        return std::string("error: cannot open file: ") + path;

    std::ostringstream ss;
    std::string line;
    int lineno = 0;
    while ( std::getline(ifd, line)) {
        lineno++;
        if ( line.find(pattern) != std::string::npos )
            ss << lineno << ": " << line << "\n";
    }

    std::string result = ss.str();
    if ( result.empty())
        return "no matches";

    return result;
}

} // namespace agent::tools
