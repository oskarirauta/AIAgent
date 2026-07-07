#include "agent/tools/grep.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <regex>
#include "common.hpp"

namespace agent::tools {

namespace {

constexpr size_t MAX_MATCHES     = 200;
constexpr size_t MAX_LINE_CHARS  = 500;
constexpr size_t MAX_TOTAL_BYTES = 100000;
constexpr size_t MAX_REGEX_CHARS = 8192;   // cap regex subject to bound backtracking

bool looks_binary(const std::string& data) {
    size_t n = std::min(data.size(), static_cast<size_t>(8000));
    if ( n == 0 )
        return false;
    size_t nonprint = 0;
    for ( size_t i = 0; i < n; ++i ) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        if ( c == 0 )
            return true;
        if ( c < 9 || ( c > 13 && c < 32 ))
            ++nonprint;
    }
    return nonprint * 100 / n > 30;
}

bool json_truthy(const JSON& v) {
    if ( v == JSON::TYPE::STRING ) {
        std::string s = common::to_lower(v.to_string());
        return s == "true" || s == "1" || s == "yes";
    }
    return v.to_bool();
}

} // namespace

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
                { "description", "regular expression to search for (ECMAScript syntax)" }
            }},
            { "ignore_case", JSON::Object{
                { "type", "boolean" },
                { "description", "case-insensitive match (optional)" }
            }},
            { "literal", JSON::Object{
                { "type", "boolean" },
                { "description", "treat pattern as a literal substring, not a regex (optional)" }
            }}
        }},
        { "required", JSON::Array{ "path", "pattern" }}
    };
}

std::string Grep::execute(const JSON& args) {
    std::string path = common::trim_ws(args["path"].to_string());
    std::string pattern = args["pattern"].to_string();

    if ( pattern.empty())
        return "error: empty pattern";

    bool ignore_case = args.contains("ignore_case") && json_truthy(args["ignore_case"]);
    bool literal = args.contains("literal") && json_truthy(args["literal"]);

    std::ifstream ifd(path, std::ios::in | std::ios::binary);
    if ( !ifd.is_open())
        return std::string("error: cannot open file: ") + path;

    // Read directly into one buffer, capped, so a huge file can't be slurped into
    // memory without bound (and to avoid the stringstream double-copy).
    constexpr size_t MAX_FILE_BYTES = 32u * 1024 * 1024;
    std::string content;
    ifd.seekg(0, std::ios::end);
    std::streamoff fsz = ifd.tellg();
    ifd.seekg(0, std::ios::beg);
    bool file_truncated = false;
    size_t to_read = ( fsz > 0 ) ? static_cast<size_t>(fsz) : 0;
    if ( to_read > MAX_FILE_BYTES ) { to_read = MAX_FILE_BYTES; file_truncated = true; }
    content.resize(to_read);
    if ( to_read )
        ifd.read(&content[0], static_cast<std::streamsize>(to_read));

    if ( looks_binary(content))
        return "error: " + path + " appears to be a binary file; not searched.";

    // Compile the regex up front so an invalid pattern is a clear error.
    std::regex re;
    if ( !literal ) {
        auto flags = std::regex::ECMAScript;
        if ( ignore_case )
            flags |= std::regex::icase;
        try {
            re.assign(pattern, flags);
        } catch ( const std::regex_error& e ) {
            return std::string("error: invalid regular expression: ") + e.what();
        }
    }

    std::string needle = ignore_case ? common::to_lower(pattern) : pattern;

    std::ostringstream ss;
    std::string line;
    long lineno = 0;
    size_t matches = 0;
    bool capped = false;

    // Iterate lines in-place over `content` (no istringstream copy of the whole file).
    for ( size_t lpos = 0; lpos <= content.size(); ) {
        size_t nl = content.find('\n', lpos);
        size_t linelen = ( nl == std::string::npos ? content.size() : nl ) - lpos;
        line.assign(content, lpos, linelen);
        if ( !line.empty() && line.back() == '\r' ) line.pop_back();
        lpos = ( nl == std::string::npos ) ? content.size() + 1 : nl + 1; // advance before any continue
        ++lineno;
        bool hit;
        if ( literal ) {
            hit = ignore_case ? ( common::to_lower(line).find(needle) != std::string::npos)
                              : ( line.find(pattern) != std::string::npos);
        } else {
            // Bound the subject length: a pathological pattern on a very long line
            // can backtrack catastrophically with no timeout. Matching the first
            // MAX_REGEX_CHARS is enough for a line-oriented search.
            hit = line.size() > MAX_REGEX_CHARS
                ? std::regex_search(line.substr(0, MAX_REGEX_CHARS), re)
                : std::regex_search(line, re);
        }
        if ( !hit )
            continue;

        std::string shown = line;
        if ( shown.size() > MAX_LINE_CHARS )
            shown = shown.substr(0, MAX_LINE_CHARS) + " …[truncated]";

        std::string entry = std::to_string(lineno) + ": " + shown + "\n";
        if ( matches >= MAX_MATCHES ||
             static_cast<size_t>(ss.tellp()) + entry.size() > MAX_TOTAL_BYTES ) {
            capped = true;
            break;
        }
        ss << entry;
        ++matches;
    }

    std::string big = file_truncated ? " (searched the first 32 MB of a larger file)" : "";
    if ( matches == 0 )
        return "no matches for " + (literal ? ("\"" + pattern + "\"") : ("/" + pattern + "/")) +
               " in " + path + big;

    std::string header = std::to_string(matches) + (matches == 1 ? " match" : " matches") +
                         (capped ? " (stopped at limit)" : "") + big + ":\n";
    return header + ss.str();
}

} // namespace agent::tools
