#include "agent/tools/edit_file.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <cctype>
#include <vector>
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
    size_t hit = 0;      // byte offset of the first replacement within `content`
    size_t hit_len = 0;  // length of the inserted new_string
};

// A numbered snippet of `content` around [from, from+len): the touched lines
// plus `ctx` lines of context each side — lets the model verify the splice
// landed correctly without re-reading the file.
std::string region_snippet(const std::string& content, size_t from, size_t len, size_t ctx = 3) {
    std::vector<size_t> starts;                      // byte offset of each line start
    starts.push_back(0);
    for ( size_t i = 0; i < content.size(); ++i )
        if ( content[i] == '\n' && i + 1 < content.size())
            starts.push_back(i + 1);

    auto line_of = [&](size_t off) {
        size_t lo = 0, hi = starts.size();
        while ( lo + 1 < hi ) {
            size_t mid = ( lo + hi ) / 2;
            if ( starts[mid] <= off ) lo = mid; else hi = mid;
        }
        return lo;                                    // 0-based line index
    };

    size_t first = line_of(from);
    size_t last = line_of(from + ( len ? len - 1 : 0 ));
    size_t begin = first > ctx ? first - ctx : 0;
    size_t end = std::min(starts.size() - 1, last + ctx);

    const size_t max_lines = 24;                      // keep the result compact
    std::string out = "[verify: lines " + std::to_string(begin + 1) + "-" +
                      std::to_string(end + 1) + "]\n";
    size_t shown = 0;
    for ( size_t l = begin; l <= end; ++l ) {
        if ( shown++ >= max_lines ) { out += "…\n"; break; }
        size_t s = starts[l];
        size_t e = content.find('\n', s);
        if ( e == std::string::npos ) e = content.size();
        std::string ln = content.substr(s, e - s);
        if ( ln.size() > 200 ) ln = ln.substr(0, 200) + "…";
        out += std::to_string(l + 1) + ": " + ln + "\n";
    }
    return out;
}

// Collapse every whitespace run to a single space and trim the edges, so an
// indentation/blank-line mismatch still compares equal.
std::string squash_ws(const std::string& s) {
    std::string out;
    bool in_ws = true; // leading whitespace dropped
    for ( unsigned char c : s ) {
        if ( std::isspace(c)) {
            if ( !in_ws ) out += ' ';
            in_ws = true;
        } else {
            out += static_cast<char>(c);
            in_ws = false;
        }
    }
    while ( !out.empty() && out.back() == ' ' )
        out.pop_back();
    return out;
}

// When old_string is not found verbatim, find the closest on-disk region so the
// model can correct its match WITHOUT re-reading the whole file. Strategy:
// compare whitespace-squashed line windows of the same line count as the needle
// and pick the window sharing the most squashed lines (first line weighted).
// Returns "" when nothing is even close.
std::string near_miss_hint(const std::string& content, const std::string& old_s) {
    auto split = [](const std::string& s) {
        std::vector<std::string> v;
        std::istringstream is(s);
        std::string l;
        while ( std::getline(is, l))
            v.push_back(l);
        if ( v.empty())
            v.push_back("");
        return v;
    };
    std::vector<std::string> file = split(content);
    std::vector<std::string> want = split(old_s);
    if ( file.size() < want.size())
        return "";

    std::vector<std::string> want_sq;
    want_sq.reserve(want.size());
    for ( const auto& l : want )
        want_sq.push_back(squash_ws(l));

    size_t best_score = 0, best_at = 0;
    for ( size_t at = 0; at + want.size() <= file.size(); ++at ) {
        size_t score = 0;
        for ( size_t k = 0; k < want.size(); ++k ) {
            if ( squash_ws(file[at + k]) == want_sq[k] )
                score += ( k == 0 ) ? 2 : 1; // anchor on the first line
        }
        if ( score > best_score ) {
            best_score = score;
            best_at = at;
        }
    }
    // Require a meaningful match: at least half the needle's lines (weighted).
    if ( best_score * 2 < want.size() + 1 )
        return "";

    size_t from = best_at;                       // 0-based
    size_t to = best_at + want.size();           // exclusive
    std::string hint = "closest on-disk region is lines " + std::to_string(from + 1) +
                       "-" + std::to_string(to) + " (differs in whitespace or small edits):\n";
    const size_t max_lines = 40;
    for ( size_t i = from; i < to && i - from < max_lines; ++i )
        hint += file[i] + "\n";
    if ( to - from > max_lines )
        hint += "…\n";
    return hint;
}

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
    if ( count == 0 ) {
        std::string err = "old_string not found (it must match exactly, including whitespace)";
        std::string hint = near_miss_hint(content, old_s);
        if ( !hint.empty())
            err += "; " + hint + "correct old_string against the text above — no need to re-read the file";
        return { false, err, {}, 0 };
    }
    if ( count > 1 && !replace_all )
        return { false, "old_string appears " + std::to_string(count) +
                        " times; add surrounding context to make it unique, or set replace_all=true", {}, 0 };

    std::string result;
    size_t replacements = 0;
    size_t first_hit = std::string::npos;
    if ( replace_all ) {
        size_t pos = 0;
        while ( pos < content.size()) {
            size_t hit = content.find(old_s, pos);
            if ( hit == std::string::npos ) { result += content.substr(pos); break; }
            if ( first_hit == std::string::npos )
                first_hit = result.size() + ( hit - pos );
            result += content.substr(pos, hit - pos) + new_s;
            pos = hit + old_s.size();
            ++replacements;
        }
    } else {
        size_t hit = content.find(old_s);
        result = content.substr(0, hit) + new_s + content.substr(hit + old_s.size());
        replacements = 1;
        first_hit = hit;
    }
    return { true, "", result, replacements,
             first_hit == std::string::npos ? 0 : first_hit, new_s.size() };
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

    // Lost-update guard: if the file changed on disk since the model read it, the
    // old_string may match a stale region — refuse and ask for a fresh read.
    if ( _tracker ) {
        std::string stale = _tracker->stale_reason(path);
        if ( !stale.empty())
            return "error: " + stale;
    }

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
    std::string verify; // numbered snippets of the touched regions

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
            if ( verify.size() < 4096 )
                verify += region_snippet(result, r.hit, r.hit_len);
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
        verify = region_snippet(result, r.hit, r.hit_len);
    }

    std::ofstream ofd(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if ( !ofd.is_open())
        return "error: cannot open file for writing: " + path;
    ofd << result;
    ofd.close();
    if ( !ofd.good())
        return "error: failed to write file: " + path;

    if ( _tracker )
        _tracker->note(path); // stamp the edited version for the next lost-update check

    std::string summary = edit_count > 1
        ? "ok: edited " + path + " (" + std::to_string(edit_count) + " edits, " +
          std::to_string(total_replacements) + " replacements)"
        : "ok: edited " + path + " (" + std::to_string(total_replacements) +
          ( total_replacements == 1 ? " replacement)" : " replacements)" );
    // The touched region(s) with context, so the model can verify the splice
    // without a follow-up read_file.
    if ( !verify.empty())
        summary += "\n" + verify;
    return summary;
}

} // namespace agent::tools
