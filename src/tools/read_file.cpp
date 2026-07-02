#include "agent/tools/read_file.hpp"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include "common.hpp"

namespace agent::tools {

static std::string normalize_text(std::string s) {
    // Replace common UTF-8 punctuation with ASCII equivalents so models with
    // weak UTF-8 handling can still make sense of the file.
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

JSON ReadFile::parameters() const {
    return JSON::Object{
        { "type", "object" },
        { "properties", JSON::Object{
            { "path", JSON::Object{
                { "type", "string" },
                { "description", "absolute or relative path to the file" }
            }}
        }},
        { "required", JSON::Array{ "path" }}
    };
}

std::string ReadFile::execute(const JSON& args) {
    std::string path = common::trim_ws(args["path"].to_string());

    std::ifstream ifd(path, std::ios::in);
    if ( !ifd.is_open())
        return std::string("error: cannot open file: ") + path;

    std::stringstream ss;
    ss << ifd.rdbuf();
    std::string content = normalize_text(ss.str());

    const size_t max_chars = 10000;
    if ( content.size() > max_chars ) {
        content = content.substr(0, max_chars);
        content += "\n\n[file truncated at " + std::to_string(max_chars) + " characters]";
    }
    return content;
}

} // namespace agent::tools
