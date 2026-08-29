#include "agent/tools/grep.hpp"

#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <regex>
#include "common.hpp"
#include "agent/gitignore.hpp"

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

bool ignored_dir(const std::string& name) {
    static const std::vector<std::string> skip = {
        ".git", ".svn", ".hg", "node_modules", "objs", "build", "dist",
        "target", ".cache", "vendor", "third_party", ".venv", "venv", "__pycache__"
    };
    for ( const auto& s : skip )
        if ( name == s )
            return true;
    return false;
}

} // namespace

JSON Grep::parameters() const {
    return JSON::Object{
        { "type", "object" },
        { "properties", JSON::Object{
            { "path", JSON::Object{
                { "type", "string" },
                { "description", "path to the file or directory subtree to search" }
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
    size_t matches = 0;
    bool capped = false;

    auto scan_file = [&](const std::filesystem::path& file, bool skip_binary_error, bool& file_truncated) {
        if ( matches >= MAX_MATCHES || capped ) { capped = true; return; }
        constexpr size_t MAX_FILE_BYTES = 32u * 1024 * 1024;
        std::ifstream ifd(file, std::ios::in | std::ios::binary);
        if ( !ifd.is_open()) {
            if ( !skip_binary_error )
                ss << "error: cannot open file: " << file.string() << "\n";
            return;
        }

        std::string content;
        ifd.seekg(0, std::ios::end);
        std::streamoff fsz = ifd.tellg();
        ifd.seekg(0, std::ios::beg);
        size_t to_read = ( fsz > 0 ) ? static_cast<size_t>(fsz) : 0;
        file_truncated = to_read > MAX_FILE_BYTES;
        if ( file_truncated )
            to_read = MAX_FILE_BYTES;
        content.resize(to_read);
        if ( to_read )
            ifd.read(&content[0], static_cast<std::streamsize>(to_read));

        if ( looks_binary(content)) {
            if ( !skip_binary_error )
                ss << "error: " << file.string() << " appears to be a binary file; not searched.\n";
            return;
        }

        std::string line;
        long lineno = 0;
        for ( size_t lpos = 0; lpos <= content.size(); ) {
            size_t nl = content.find('\n', lpos);
            size_t linelen = ( nl == std::string::npos ? content.size() : nl ) - lpos;
            line.assign(content, lpos, linelen);
            if ( !line.empty() && line.back() == '\r' ) line.pop_back();
            lpos = ( nl == std::string::npos ) ? content.size() + 1 : nl + 1;
            ++lineno;
            bool hit;
            if ( literal ) {
                hit = ignore_case ? ( common::to_lower(line).find(needle) != std::string::npos)
                                  : ( line.find(pattern) != std::string::npos);
            } else {
                hit = line.size() > MAX_REGEX_CHARS
                    ? std::regex_search(line.substr(0, MAX_REGEX_CHARS), re)
                    : std::regex_search(line, re);
            }
            if ( !hit )
                continue;

            std::string shown = line;
            if ( shown.size() > MAX_LINE_CHARS )
                shown = shown.substr(0, MAX_LINE_CHARS) + " …[truncated]";

            std::string entry = file.string() + ":" + std::to_string(lineno) + ": " + shown + "\n";
            if ( matches >= MAX_MATCHES ||
                 static_cast<size_t>(ss.tellp()) + entry.size() > MAX_TOTAL_BYTES ) {
                capped = true;
                return;
            }
            ss << entry;
            ++matches;
        }
    };

    std::error_code ec;
    std::filesystem::path root(path);
    if ( std::filesystem::is_regular_file(root, ec)) {
        bool truncated = false;
        scan_file(root, false, truncated);
        std::string big = truncated ? " (searched the first 32 MB of a larger file)" : "";
        if ( matches == 0 && ss.str().empty())
            return "no matches for " + (literal ? ("\"" + pattern + "\"") : ("/" + pattern + "/")) +
                   " in " + path + big;
        std::string out = ss.str();
        if ( out.rfind("error:", 0) == 0 )
            return out;
        if ( matches == 0 )
            return "no matches for " + (literal ? ("\"" + pattern + "\"") : ("/" + pattern + "/")) +
                   " in " + path + big;
        std::string header = std::to_string(matches) + (matches == 1 ? " match" : " matches") +
                             (capped ? " (stopped at limit)" : "") + big + ":\n";
        return header + out;
    }

    if ( !std::filesystem::is_directory(root, ec))
        return "error: path does not exist: " + path;

    agent::GitIgnore gi;
    gi.load(path);
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec), end;
    for ( ; it != end && !capped; it.increment(ec)) {
        if ( ec ) break;
        const auto& entry = *it;
        std::error_code dec;
        std::string rel = entry.path().lexically_relative(root).generic_string();
        if ( entry.is_directory(dec)) {
            if ( ignored_dir(entry.path().filename().string()) || gi.ignored(rel, true))
                it.disable_recursion_pending();
            continue;
        }
        if ( entry.is_regular_file(dec)) {
            if ( gi.ignored(rel, false))
                continue;
            bool truncated = false;
            scan_file(entry.path(), true, truncated);
        }
    }

    if ( matches == 0 && ss.str().empty())
        return "no matches for " + (literal ? ("\"" + pattern + "\"") : ("/" + pattern + "/")) +
               " under " + path;

    std::string out = ss.str();
    if ( out.rfind("error:", 0) == 0 )
        return out;

    if ( matches == 0 )
        return "no matches for " + (literal ? ("\"" + pattern + "\"") : ("/" + pattern + "/")) +
               " under " + path;

    std::string header = std::to_string(matches) + (matches == 1 ? " match" : " matches") +
                         (capped ? " (stopped at limit)" : "") + " under " + path + ":\n";
    return header + out;
}

} // namespace agent::tools
