#include "agent/text_utils.hpp"

#include <cstring>
#include <vector>
#include <sstream>
#include <algorithm>

namespace agent {

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


std::string normalize_text(std::string s) {
    struct Replacement {
        const char* utf8;
        const char* ascii;
    };
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
    for ( const auto& r : reps ) {
        size_t pos = 0;
        while ((pos = s.find(r.utf8, pos)) != std::string::npos) {
            s.replace(pos, std::strlen(r.utf8), r.ascii);
            pos += std::strlen(r.ascii);
        }
    }
    return s;
}

} // namespace agent
