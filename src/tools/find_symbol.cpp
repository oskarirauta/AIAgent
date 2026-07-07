#include "agent/tools/find_symbol.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <regex>
#include <algorithm>
#include <cctype>
#include "common.hpp"
#include "agent/gitignore.hpp"

namespace agent::tools {

namespace {

constexpr size_t MAX_MATCHES     = 100;
constexpr size_t MAX_FILES       = 6000;   // scanned-file cap
constexpr size_t MAX_FILE_BYTES  = 1000000; // skip files larger than this
constexpr size_t MAX_LINE_CHARS  = 300;
constexpr size_t MAX_TOTAL_BYTES = 60000;

// Directories that are never source and only slow the walk / add noise.
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

std::string regex_escape(const std::string& s) {
    std::string out;
    for ( char c : s ) {
        if ( std::string(".^$|()[]{}*+?\\").find(c) != std::string::npos )
            out += '\\';
        out += c;
    }
    return out;
}

} // namespace

std::string FindSymbol::description() const {
    return "Find where a symbol (function, class, struct, enum, type, trait, …) is "
           "DEFINED across the project, without knowing the file. Definition-aware and "
           "tree-wide — more precise than grepping every file for a name that also "
           "appears at call sites. Give the exact identifier in `name`; optionally "
           "restrict to a subtree with `path`. Returns matching definitions as "
           "file:line with the defining line.";
}

JSON FindSymbol::parameters() const {
    return JSON::Object{
        { "type", "object" },
        { "properties", JSON::Object{
            { "name", JSON::Object{
                { "type", "string" },
                { "description", "the exact symbol name to locate the definition of" }
            }},
            { "path", JSON::Object{
                { "type", "string" },
                { "description", "directory subtree to search (optional, defaults to the working directory)" }
            }}
        }},
        { "required", JSON::Array{ "name" }}
    };
}

std::string FindSymbol::execute(const JSON& args) {
    std::string sym = common::trim_ws(args.contains("name") ? args["name"].to_string() : "");
    if ( sym.empty())
        return "error: provide a symbol `name`";
    // A symbol is a single identifier; reject anything with whitespace/separators
    // so the regexes below stay anchored to a real name.
    for ( char c : sym )
        if ( !( std::isalnum(static_cast<unsigned char>(c)) || c == '_' ))
            return "error: `name` must be a single identifier (letters, digits, underscore)";

    std::string root = ".";
    if ( args.contains("path")) {
        std::string p = common::trim_ws(args["path"].to_string());
        if ( !p.empty())
            root = p;
    }
    std::error_code ec;
    if ( !std::filesystem::exists(root, ec))
        return "error: path does not exist: " + root;

    std::string esc = regex_escape(sym);
    // Definition led by a language keyword (class/struct/def/fn/type/…), the name
    // appearing somewhere after it on the line.
    std::regex kw_re(
        "\\b(class|struct|enum|union|interface|trait|namespace|def|fn|func|function|"
        "type|typedef|impl|module|macro|package|record|protocol|object)\\b[^\\n]*\\b" + esc + "\\b");
    // C-family definition/opening: NAME( ... ) not terminated by ';' on the line
    // (calls and prototypes end with ';'), so this favours a real definition.
    std::regex call_re("\\b" + esc + "\\s*\\(");

    std::ostringstream out;
    size_t matches = 0, files = 0;
    bool capped = false;

    auto scan_file = [&](const std::filesystem::path& file) {
        if ( matches >= MAX_MATCHES || files >= MAX_FILES ) { capped = true; return; }
        std::error_code fec;
        auto sz = std::filesystem::file_size(file, fec);
        if ( fec || sz > MAX_FILE_BYTES )
            return;
        std::ifstream ifd(file, std::ios::in | std::ios::binary);
        if ( !ifd.is_open())
            return;
        std::stringstream buf;
        buf << ifd.rdbuf();
        std::string content = buf.str();
        if ( looks_binary(content))
            return;
        ++files;

        std::istringstream lines(content);
        std::string line;
        long lineno = 0;
        while ( std::getline(lines, line)) {
            ++lineno;
            // Cheap pre-filter: both patterns require the symbol name, so a line
            // without it as a substring can't match — skip the two regex_search.
            if ( line.find(sym) == std::string::npos )
                continue;
            bool hit = std::regex_search(line, kw_re);
            if ( !hit && line.find(';') == std::string::npos && std::regex_search(line, call_re)) {
                // A NAME( opening with no semicolon — likely a definition, not a call.
                hit = true;
            }
            if ( !hit )
                continue;

            std::string shown = common::trim_ws(line);
            if ( shown.size() > MAX_LINE_CHARS )
                shown = shown.substr(0, MAX_LINE_CHARS) + " …";
            std::string entry = file.string() + ":" + std::to_string(lineno) + ":  " + shown + "\n";
            if ( matches >= MAX_MATCHES ||
                 static_cast<size_t>(out.tellp()) + entry.size() > MAX_TOTAL_BYTES ) {
                capped = true;
                return;
            }
            out << entry;
            ++matches;
        }
    };

    if ( std::filesystem::is_regular_file(root, ec)) {
        scan_file(root);
    } else {
        agent::GitIgnore gi;
        gi.load(root);
        std::filesystem::recursive_directory_iterator it(
            root, std::filesystem::directory_options::skip_permission_denied, ec), end;
        for ( ; it != end && !capped; it.increment(ec)) {
            if ( ec ) break;
            const auto& entry = *it;
            std::error_code dec;
            std::string rel = std::filesystem::relative(entry.path(), root, dec).generic_string();
            if ( entry.is_directory(dec)) {
                if ( ignored_dir(entry.path().filename().string()) || gi.ignored(rel, true))
                    it.disable_recursion_pending();
                continue;
            }
            if ( entry.is_regular_file(dec)) {
                if ( gi.ignored(rel, false))
                    continue;
                scan_file(entry.path());
            }
        }
    }

    if ( matches == 0 )
        return "no definition of `" + sym + "` found under " + root +
               "\n(the symbol may be external, or defined in an unusual form — try grep)";

    std::string header = std::to_string(matches) +
                         ( matches == 1 ? " definition" : " definitions" ) +
                         " of `" + sym + "`" + ( capped ? " (stopped at limit)" : "" ) + ":\n";
    return header + out.str();
}

} // namespace agent::tools
