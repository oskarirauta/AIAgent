#include "agent/tools/find_references.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include "common.hpp"
#include "agent/gitignore.hpp"

namespace agent::tools {

namespace {

constexpr size_t MAX_MATCHES     = 200;
constexpr size_t MAX_FILES       = 6000;
constexpr size_t MAX_FILE_BYTES  = 1000000;
constexpr size_t MAX_LINE_CHARS  = 300;
constexpr size_t MAX_TOTAL_BYTES = 60000;

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

bool ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Count whole-word occurrences of `sym` on a line (identifier boundaries).
size_t whole_word_hits(const std::string& line, const std::string& sym) {
    size_t hits = 0, pos = 0;
    while (( pos = line.find(sym, pos)) != std::string::npos ) {
        bool left = ( pos == 0 ) || !ident_char(line[pos - 1]);
        size_t after = pos + sym.size();
        bool right = ( after >= line.size()) || !ident_char(line[after]);
        if ( left && right )
            ++hits;
        pos = after;
    }
    return hits;
}

} // namespace

std::string FindReferences::description() const {
    return "Find where a symbol is USED across the project — its references / call "
           "sites — as whole-identifier matches (so `foo` does not match `foobar`). This "
           "is the usage counterpart to find_symbol (which finds the definition) and is "
           "more precise than grep's substring match. Give the exact identifier in "
           "`name`; optionally restrict to a subtree with `path`. Returns file:line with "
           "the referencing line, and a total count.";
}

JSON FindReferences::parameters() const {
    return JSON::Object{
        { "type", "object" },
        { "properties", JSON::Object{
            { "name", JSON::Object{
                { "type", "string" },
                { "description", "the exact symbol name to find references to" }
            }},
            { "path", JSON::Object{
                { "type", "string" },
                { "description", "directory subtree to search (optional, defaults to the working directory)" }
            }}
        }},
        { "required", JSON::Array{ "name" }}
    };
}

std::string FindReferences::execute(const JSON& args) {
    std::string sym = common::trim_ws(args.contains("name") ? args["name"].to_string() : "");
    if ( sym.empty())
        return "error: provide a symbol `name`";
    for ( char c : sym )
        if ( !ident_char(c))
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

    std::ostringstream out;
    size_t matches = 0, files = 0, total_refs = 0;
    bool capped = false;

    auto scan_file = [&](const std::filesystem::path& file) {
        if ( matches >= MAX_MATCHES || files >= MAX_FILES ) { capped = true; return; }
        std::error_code fec;
        auto sz = std::filesystem::file_size(file, fec);
        if ( fec || sz > MAX_FILE_BYTES )
            return;
        // Quick reject: the symbol isn't in the file at all.
        std::ifstream ifd(file, std::ios::in | std::ios::binary);
        if ( !ifd.is_open())
            return;
        std::stringstream buf;
        buf << ifd.rdbuf();
        std::string content = buf.str();
        if ( content.find(sym) == std::string::npos || looks_binary(content))
            return;
        ++files;

        std::istringstream lines(content);
        std::string line;
        long lineno = 0;
        while ( std::getline(lines, line)) {
            ++lineno;
            size_t hits = whole_word_hits(line, sym);
            if ( hits == 0 )
                continue;
            total_refs += hits;
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
            // lexically_relative is a pure path operation — no filesystem I/O per
            // entry (unlike std::filesystem::relative, which weakly-canonicalises).
            // entry.path() is always under `root` (recursive iterator from root).
            std::string rel = entry.path().lexically_relative(root).generic_string();
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
        return "no references to `" + sym + "` found under " + root;

    std::string header = std::to_string(total_refs) +
                         ( total_refs == 1 ? " reference" : " references" ) +
                         " to `" + sym + "` on " + std::to_string(matches) +
                         ( matches == 1 ? " line" : " lines" ) +
                         ( capped ? " (stopped at limit)" : "" ) + ":\n";
    return header + out.str();
}

} // namespace agent::tools
