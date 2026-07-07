#include "agent/text_utils.hpp"

#include <cstring>
#include <cctype>
#include <vector>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <regex>
#include <utility>

namespace agent {

std::string redact_secrets(const std::string& text, int& count) {
    count = 0;
    if ( text.empty())
        return text;
    std::string out = text;

    // Ordered fixed-format secret patterns. Each match becomes its replacement.
    // sk-ant- must precede sk- (the hyphenated form would otherwise slip past).
    static const std::vector<std::pair<std::regex, std::string>> rules = {
        { std::regex(R"(-----BEGIN[A-Z ]*PRIVATE KEY-----[\s\S]*?-----END[A-Z ]*PRIVATE KEY-----)"),
          "[REDACTED PRIVATE KEY]" },
        { std::regex(R"(sk-ant-[A-Za-z0-9_-]{20,})"), "[REDACTED]" },
        // Allow the internal separators of modern OpenAI keys (sk-proj-, sk-svcacct-,
        // sk-admin-, sk-None-…) as well as the legacy all-alphanumeric body.
        { std::regex(R"(sk-[A-Za-z0-9_-]{20,})"), "[REDACTED]" },
        { std::regex(R"(gh[posru]_[A-Za-z0-9]{20,})"), "[REDACTED]" },
        { std::regex(R"(github_pat_[A-Za-z0-9_]{20,})"), "[REDACTED]" },
        { std::regex(R"(xox[baprs]-[A-Za-z0-9-]{10,})"), "[REDACTED]" },
        { std::regex(R"(AKIA[0-9A-Z]{16})"), "[REDACTED]" },
        { std::regex(R"(AIza[0-9A-Za-z_-]{35})"), "[REDACTED]" },
        { std::regex(R"(glpat-[A-Za-z0-9_-]{20})"), "[REDACTED]" },
        { std::regex(R"(eyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,})"), "[REDACTED JWT]" },
        { std::regex(R"([Bb]earer\s+[A-Za-z0-9._~+/-]{16,}=*)"), "Bearer [REDACTED]" },
    };
    for ( const auto& [re, rep] : rules ) {
        int n = static_cast<int>(std::distance(std::sregex_iterator(out.begin(), out.end(), re),
                                               std::sregex_iterator()));
        if ( n > 0 ) {
            count += n;
            out = std::regex_replace(out, re, rep);
        }
    }

    // Labelled assignments: `SECRET=<value>`, `api_key: "<value>"`, etc. Redact the
    // value, keeping the label. Skips obvious placeholders so template/.env.example
    // files don't get noisily rewritten.
    {
        static const std::regex re(
            R"((password|passwd|secret|api[_-]?key|apikey|access[_-]?key|secret[_-]?key|auth[_-]?token|token|private[_-]?key)("?\s*[:=]\s*"?)([^\s"']{8,}))",
            std::regex::icase);
        std::string res;
        auto begin = std::sregex_iterator(out.begin(), out.end(), re);
        auto end = std::sregex_iterator();
        size_t last = 0;
        for ( auto it = begin; it != end; ++it ) {
            const std::smatch& m = *it;
            std::string val = m[3].str();
            std::string low = val;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            bool placeholder = low.find("your") != std::string::npos ||
                               low.find("xxx") != std::string::npos ||
                               low.find("example") != std::string::npos ||
                               low.find("changeme") != std::string::npos ||
                               low.find("redacted") != std::string::npos ||
                               low.find("<") != std::string::npos;
            res.append(out, last, static_cast<size_t>(m.position()) - last);
            if ( placeholder ) {
                res += m.str(); // leave it as-is
            } else {
                res += m[1].str() + m[2].str() + "[REDACTED]";
                ++count;
            }
            last = static_cast<size_t>(m.position()) + static_cast<size_t>(m.length());
        }
        res.append(out, last, std::string::npos);
        out = std::move(res);
    }

    return out;
}

namespace {

constexpr size_t MENTION_FILE_BYTES  = 64 * 1024;   // per-file cap
constexpr size_t MENTION_TOTAL_BYTES = 192 * 1024;  // per-message cap across mentions

// Read up to `cap` bytes of a file; returns false if it cannot be opened.
bool read_capped(const std::string& path, size_t cap, std::string& out, bool& truncated) {
    std::ifstream ifd(path, std::ios::in | std::ios::binary);
    if ( !ifd.is_open())
        return false;
    out.resize(cap + 1);
    ifd.read(&out[0], static_cast<std::streamsize>(cap + 1));
    size_t got = static_cast<size_t>(ifd.gcount());
    truncated = got > cap;
    out.resize(std::min(got, cap));
    if ( truncated ) {
        // Cut at the last full line so the block ends cleanly.
        size_t nl = out.rfind('\n');
        if ( nl != std::string::npos && nl > 0 )
            out.resize(nl + 1);
    }
    return true;
}

} // namespace

std::string expand_file_mentions(const std::string& text, std::vector<FileMention>* mentions) {
    std::string out;
    out.reserve(text.size());
    size_t total = 0;
    size_t i = 0;
    while ( i < text.size()) {
        char c = text[i];
        bool at_token_start = ( c == '@' ) &&
            ( i == 0 || std::isspace(static_cast<unsigned char>(text[i - 1])));
        if ( !at_token_start ) {
            out += c;
            ++i;
            continue;
        }
        // Token runs to the next whitespace.
        size_t end = i + 1;
        while ( end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])))
            ++end;
        std::string path = text.substr(i + 1, end - ( i + 1 ));

        // Tolerate trailing punctuation: strip until the path exists (or give up).
        std::error_code ec;
        auto is_file = [&ec](const std::string& p) {
            return !p.empty() && std::filesystem::is_regular_file(p, ec);
        };
        size_t stripped = 0;
        while ( !is_file(path) && !path.empty() &&
                std::string(",.;:!?)]}\"'").find(path.back()) != std::string::npos ) {
            path.pop_back();
            ++stripped;
        }

        std::string content;
        bool truncated = false;
        if ( !is_file(path) || total >= MENTION_TOTAL_BYTES ||
             !read_capped(path, std::min(MENTION_FILE_BYTES, MENTION_TOTAL_BYTES - total),
                          content, truncated)) {
            // Not a real mention (or out of budget): leave the token untouched.
            out.append(text, i, end - i);
            i = end;
            continue;
        }
        total += content.size();

        size_t lines = static_cast<size_t>(std::count(content.begin(), content.end(), '\n'));
        if ( !content.empty() && content.back() != '\n' )
            ++lines;
        if ( mentions )
            mentions->push_back({ path, lines, truncated });

        out += "--- file: " + path + " (" + std::to_string(lines) + " lines" +
               ( truncated ? ", truncated" : "" ) + ") ---\n";
        out += content;
        if ( !content.empty() && content.back() != '\n' )
            out += '\n';
        if ( truncated )
            out += "…(truncated — use read_file with an offset for the rest)\n";
        out += "--- end of " + path + " ---";
        // Re-attach any punctuation that was stripped from the token.
        out.append(text, end - stripped, stripped);
        i = end;
    }
    return out;
}

bool has_ultra_keyword(const std::string& s) {
    std::string lo;
    lo.reserve(s.size());
    for ( unsigned char c : s ) lo += static_cast<char>(std::tolower(c));
    for ( const char* kw : { "ultracode", "ultrathink" }) {
        std::string k = kw;
        size_t pos = 0;
        while (( pos = lo.find(k, pos)) != std::string::npos ) {
            bool lb = ( pos == 0 ) || !std::isalnum(static_cast<unsigned char>(lo[pos - 1]));
            size_t end = pos + k.size();
            bool rb = ( end >= lo.size()) || !std::isalnum(static_cast<unsigned char>(lo[end]));
            if ( lb && rb )
                return true;
            pos = end;
        }
    }
    return false;
}

std::string block_diff(const std::string& old_text, const std::string& new_text,
                       const std::string& from_label, const std::string& to_label) {
    auto split = [](const std::string& s) {
        std::vector<std::string> v; std::string line; std::istringstream is(s);
        while ( std::getline(is, line)) v.push_back(line);
        return v;
    };
    std::vector<std::string> o = split(old_text), n = split(new_text);
    size_t p = 0;
    while ( p < o.size() && p < n.size() && o[p] == n[p] ) ++p;
    size_t s = 0;
    while ( s < o.size() - p && s < n.size() - p && o[o.size() - 1 - s] == n[n.size() - 1 - s] ) ++s;

    std::string out = "--- " + from_label + "\n+++ " + to_label + "\n";
    size_t ctx_start = p > 2 ? p - 2 : 0;
    for ( size_t i = ctx_start; i < p; ++i ) out += "  " + o[i] + "\n";
    size_t shown = 0;
    for ( size_t i = p; i < o.size() - s && shown < 200; ++i, ++shown ) out += "- " + o[i] + "\n";
    shown = 0;
    for ( size_t i = p; i < n.size() - s && shown < 200; ++i, ++shown ) out += "+ " + n[i] + "\n";
    size_t ctx_end = std::min(o.size(), ( o.size() - s ) + 2 );
    for ( size_t i = o.size() - s; i < ctx_end; ++i ) out += "  " + o[i] + "\n";
    return out;
}


namespace {
struct Replacement {
    const char* utf8;
    const char* ascii;
};
const Replacement* normalize_reps(size_t& count) {
    static const Replacement reps[] = {
        { "\xe2\x80\x94", "--" },   // em dash
        { "\xe2\x80\x93", "-" },    // en dash
        { "\xe2\x80\x90", "-" },    // hyphen
        { "\xe2\x80\x91", "-" },    // non-breaking hyphen
        { "\xe2\x80\x92", "-" },    // figure dash
        { "\xe2\x80\x98", "'" },    // left single quotation mark
        { "\xe2\x80\x99", "'" },    // right single quotation mark
        { "\xe2\x80\x9a", "," },    // single low-9 quotation mark
        { "\xe2\x80\x9b", "'" },    // single high-reversed-9 quotation mark
        { "\xe2\x80\x9c", "\"" },  // left double quotation mark
        { "\xe2\x80\x9d", "\"" },  // right double quotation mark
        { "\xe2\x80\x9e", "\"" },  // double low-9 quotation mark
        { "\xe2\x80\x9f", "\"" },  // double high-reversed-9 quotation mark
        { "\xe2\x80\xa6", "..." },  // horizontal ellipsis
        { "\xe2\x80\xa2", "*" },    // bullet
        { "\xe2\x80\xa3", "*" },    // triangular bullet
        { "\xe2\x80\xb9", "<" },    // single left-pointing angle quotation mark
        { "\xe2\x80\xba", ">" },    // single right-pointing angle quotation mark
        { "\xe2\x86\x90", "<-" },   // leftwards arrow
        { "\xe2\x86\x92", "->" },   // rightwards arrow
        { "\xe2\x86\x91", "^" },    // upwards arrow
        { "\xe2\x86\x93", "v" },    // downwards arrow
        { "\xe2\x86\x94", "<->" },  // left right arrow
        { "\xe2\x88\x92", "-" },    // minus sign
        { "\xe2\x89\x88", "~" },    // almost equal to
        { "\xe2\x89\xa0", "!=" },   // not equal to
        { "\xe2\x89\xa4", "<=" },   // less-than or equal to
        { "\xe2\x89\xa5", ">=" },   // greater-than or equal to
        { "\xe2\x9c\x85", "[x]" },  // white heavy check mark
        { "\xe2\x9c\x93", "[x]" },  // check mark
        { "\xe2\x9c\x94", "[x]" },  // heavy check mark
        { "\xe2\x9c\x97", "[ ]" },  // ballot x
        { "\xe2\x9a\xa0", "(!)" },  // warning sign
        { "\xe2\x84\xa2", "(TM)" },  // trade mark sign
        { "\xc2\xab", "<<" },       // left-pointing double angle quotation mark
        { "\xc2\xbb", ">>" },       // right-pointing double angle quotation mark
        { "\xc2\xa9", "(C)" },      // copyright sign
        { "\xc2\xae", "(R)" },      // registered sign
        { "\xc2\xa0", " " },        // non-breaking space
        { "\xc2\xad", "" },         // soft hyphen
        { "\xef\xbb\xbf", "" },      // zero width no-break space (BOM)
    };
    count = sizeof(reps) / sizeof(reps[0]);
    return reps;
}
} // namespace

std::string normalize_text(std::string s) {
    size_t count = 0;
    const Replacement* reps = normalize_reps(count);
    for ( size_t ri = 0; ri < count; ++ri ) {
        const Replacement& r = reps[ri];
        size_t pos = 0;
        while ((pos = s.find(r.utf8, pos)) != std::string::npos) {
            s.replace(pos, std::strlen(r.utf8), r.ascii);
            pos += std::strlen(r.ascii);
        }
    }
    return s;
}

// Single left-to-right pass equivalent to normalize_text (the replacements emit
// only ASCII, so pass order is irrelevant), additionally recording for each output
// byte the source index it came from. `index_map` ends with a sentinel = s.size(),
// so a match at output [a,b) maps to raw [index_map[a], index_map[b]).
std::string normalize_text_mapped(const std::string& s, std::vector<size_t>& index_map) {
    size_t count = 0;
    const Replacement* reps = normalize_reps(count);
    std::string out;
    index_map.clear();
    out.reserve(s.size());
    index_map.reserve(s.size() + 1);
    for ( size_t i = 0; i < s.size(); ) {
        const Replacement* hit = nullptr;
        for ( size_t ri = 0; ri < count; ++ri )
            if ( s.compare(i, std::strlen(reps[ri].utf8), reps[ri].utf8) == 0 ) { hit = &reps[ri]; break; }
        if ( hit ) {
            for ( size_t k = 0; hit->ascii[k]; ++k ) { out += hit->ascii[k]; index_map.push_back(i); }
            i += std::strlen(hit->utf8);
        } else {
            out += s[i]; index_map.push_back(i); ++i;
        }
    }
    index_map.push_back(s.size());
    return out;
}

} // namespace agent
