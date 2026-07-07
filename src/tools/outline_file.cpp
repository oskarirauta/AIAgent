#include "agent/tools/outline_file.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <regex>
#include <cctype>
#include "common.hpp"

namespace agent::tools {

namespace {

constexpr size_t MAX_FILE_BYTES = 2 * 1024 * 1024;
constexpr size_t MAX_ENTRIES    = 500;

// A line led by a definition keyword (class/struct/def/fn/type/…), the keyword
// appearing at the start of the trimmed line.
bool is_keyword_def(const std::string& t) {
    static const std::regex kw(
        "^(export\\s+|public\\s+|private\\s+|protected\\s+|static\\s+|final\\s+|"
        "abstract\\s+|async\\s+|pub\\s+|inline\\s+|virtual\\s+|extern\\s+)*"
        "(class|struct|enum|union|interface|trait|namespace|def|fn|func|function|"
        "type|typedef|impl|module|macro|package|record|protocol|object)\\b");
    return std::regex_search(t, kw);
}

// A C-family function signature / opening: NAME( … ) that isn't a call or
// prototype (those end in ';') and isn't a control statement.
bool is_signature(const std::string& t) {
    size_t paren = t.find('(');
    if ( paren == std::string::npos || t.empty() || t.back() == ';' )
        return false;
    // Comment or preprocessor lines are not signatures.
    if ( t[0] == '#' || t[0] == '*' || t.rfind("//", 0) == 0 || t.rfind("/*", 0) == 0 )
        return false;

    std::string first = t.substr(0, t.find_first_of(" \t("));
    static const std::set<std::string> ctrl = {
        "if", "for", "while", "switch", "return", "else", "catch", "do",
        "sizeof", "assert", "case", "throw", "with", "elif", "except", "match"
    };
    if ( ctrl.count(first))
        return false;

    // An identifier must sit immediately before '('.
    size_t e = paren;
    while ( e > 0 && std::isspace(static_cast<unsigned char>(t[e - 1]))) --e;
    size_t bstart = e;
    while ( bstart > 0 && ( std::isalnum(static_cast<unsigned char>(t[bstart - 1])) || t[bstart - 1] == '_' ))
        --bstart;
    if ( bstart == e )
        return false;

    // Bias toward definitions: a return type / qualifier before the name, or an
    // opening brace / bare signature end.
    bool has_qualifier = t.find_first_of(" \t") < bstart;
    bool opens = t.back() == '{' || t.back() == ')' || t.back() == ':';
    return has_qualifier || opens;
}

std::string trim(const std::string& s) { return common::trim_ws(s); }

} // namespace

JSON OutlineFile::parameters() const {
    return JSON::Object{
        { "type", "object" },
        { "properties", JSON::Object{
            { "path", JSON::Object{
                { "type", "string" },
                { "description", "the file to outline" }
            }}
        }},
        { "required", JSON::Array{ "path" } }
    };
}

std::string OutlineFile::execute(const JSON& args) {
    std::string path = common::trim_ws(args.contains("path") ? args["path"].to_string() : "");
    if ( path.empty())
        return "error: provide a file `path`";
    std::error_code ec;
    if ( !std::filesystem::is_regular_file(path, ec))
        return "error: not a file: " + path;
    if ( std::filesystem::file_size(path, ec) > MAX_FILE_BYTES )
        return "error: file too large to outline (" + path + ")";

    std::ifstream ifd(path, std::ios::in | std::ios::binary);
    if ( !ifd.is_open())
        return "error: cannot read file: " + path;

    std::string line;
    size_t lineno = 0;
    std::ostringstream out;
    size_t entries = 0;
    bool capped = false;

    while ( std::getline(ifd, line)) {
        ++lineno;
        std::string t = trim(line);
        if ( t.empty())
            continue;
        if ( is_keyword_def(t) || is_signature(t)) {
            if ( entries++ >= MAX_ENTRIES ) { capped = true; break; }
            if ( t.size() > 160 ) t = t.substr(0, 160) + "…";
            out << "  " << lineno << ": " << t << "\n";
        }
    }

    if ( entries == 0 )
        return "outline of " + path + ": no definitions found (it may be data, or a "
               "language this heuristic doesn't cover — read_file it directly)";

    std::string head = "outline of " + path + " (" + std::to_string(entries) +
                       ( entries == 1 ? " definition" : " definitions" ) + "):\n";
    std::string res = head + out.str();
    if ( capped )
        res += "\n(stopped at " + std::to_string(MAX_ENTRIES) + " — the file has more)\n";
    return res;
}

} // namespace agent::tools
