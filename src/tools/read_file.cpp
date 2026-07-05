#include "agent/tools/read_file.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include "common.hpp"
#include "agent/text_utils.hpp"

namespace agent::tools {

namespace {

constexpr size_t DEFAULT_LINES   = 2000;    // lines returned when no limit is given
constexpr size_t MAX_LINE_CHARS  = 2000;    // per-line cap (avoids one huge line flooding context)
constexpr size_t MAX_TOTAL_BYTES = 200000;  // overall output cap

// Heuristic binary detection: a NUL byte, or a high share of control bytes.
bool looks_binary(const std::string& data) {
    size_t n = std::min(data.size(), static_cast<size_t>(8000));
    if ( n == 0 )
        return false;
    size_t nonprint = 0;
    for ( size_t i = 0; i < n; ++i ) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        if ( c == 0 )
            return true;
        // control chars other than tab/newline/vtab/formfeed/carriage-return
        if ( c < 9 || ( c > 13 && c < 32 ))
            ++nonprint;
    }
    return nonprint * 100 / n > 30;
}

long json_long(const JSON& v, long fallback) {
    if ( v == JSON::TYPE::INT ) return static_cast<long>(static_cast<long long>(v));
    if ( v == JSON::TYPE::FLOAT ) return static_cast<long>(static_cast<long double>(v));
    return fallback;
}

} // namespace

JSON ReadFile::parameters() const {
    return JSON::Object{
        { "type", "object" },
        { "properties", JSON::Object{
            { "path", JSON::Object{
                { "type", "string" },
                { "description", "absolute or relative path to the file" }
            }},
            { "offset", JSON::Object{
                { "type", "integer" },
                { "description", "1-based line number to start reading from (optional)" }
            }},
            { "limit", JSON::Object{
                { "type", "integer" },
                { "description", "maximum number of lines to read (optional)" }
            }}
        }},
        { "required", JSON::Array{ "path" }}
    };
}

std::string ReadFile::execute(const JSON& args) {
    std::string path = common::trim_ws(args["path"].to_string());

    std::ifstream ifd(path, std::ios::in | std::ios::binary);
    if ( !ifd.is_open())
        return std::string("error: cannot open file: ") + path;

    std::stringstream ss;
    ss << ifd.rdbuf();
    std::string raw = ss.str();

    if ( raw.empty())
        return "(empty file)";

    if ( looks_binary(raw))
        return "error: " + path + " appears to be a binary file (" +
               std::to_string(raw.size()) + " bytes); not shown.";

    std::string content = agent::normalize_text(raw);

    // Split into lines (keep it simple: split on '\n').
    std::vector<std::string> lines;
    {
        std::string line;
        std::istringstream ls(content);
        while ( std::getline(ls, line))
            lines.push_back(line);
    }
    long total = static_cast<long>(lines.size());

    long offset = args.contains("offset") ? json_long(args["offset"], 1) : 1;
    long limit  = args.contains("limit")  ? json_long(args["limit"], DEFAULT_LINES)
                                          : static_cast<long>(DEFAULT_LINES);
    if ( offset < 1 ) offset = 1;
    if ( limit < 1 ) limit = 1;
    if ( offset > total )
        return "error: offset " + std::to_string(offset) + " is past the end of the file (" +
               std::to_string(total) + " lines)";

    long start = offset - 1;
    long end = std::min<long>(total, start + limit);

    std::string out;
    bool byte_capped = false;
    long shown_end = start;
    for ( long i = start; i < end; ++i ) {
        std::string ln = lines[i];
        if ( ln.size() > MAX_LINE_CHARS )
            ln = ln.substr(0, MAX_LINE_CHARS) + " …[line truncated]";
        if ( out.size() + ln.size() + 1 > MAX_TOTAL_BYTES ) {
            byte_capped = true;
            break;
        }
        if ( !out.empty()) out += "\n";
        out += ln;
        shown_end = i + 1;
    }

    bool partial = ( start > 0 || shown_end < total );
    std::string header;
    if ( partial )
        header = "[lines " + std::to_string(start + 1) + "-" + std::to_string(shown_end) +
                 " of " + std::to_string(total) + "]\n";

    std::string footer;
    if ( shown_end < total )
        footer = "\n[" + std::to_string(total - shown_end) + " more lines" +
                 (byte_capped ? " (stopped at output cap)" : "") +
                 "; read with offset " + std::to_string(shown_end + 1) + "]";

    return header + out + footer;
}

} // namespace agent::tools
