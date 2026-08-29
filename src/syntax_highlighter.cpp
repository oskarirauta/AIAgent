#include "agent/syntax_highlighter.hpp"

#include <cctype>
#include <unordered_set>

namespace agent {

SyntaxHighlighter::SyntaxHighlighter(int base_color_pairs) {
    // color pairs are initialized by the caller (NcursesRepl::setup)
    _keyword_pair = base_color_pairs;
    _string_pair = base_color_pairs + 1;
    _comment_pair = base_color_pairs + 2;
    _number_pair = base_color_pairs + 3;
    _type_pair = base_color_pairs + 4;
    _fence_pair = base_color_pairs + 5;
}

Language SyntaxHighlighter::detect(const std::string& fence) const {
    std::string s;
    for ( char c : fence ) {
        if ( c == '`' || c == ' ' || c == '\t' )
            continue;
        s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if ( s == "json" ) return Language::json;
    if ( s == "c" || s == "cpp" || s == "c++" || s == "cxx" || s == "h" || s == "hpp" )
        return Language::cpp;
    if ( s == "js" || s == "javascript" || s == "ts" || s == "typescript" )
        return Language::javascript;
    if ( s == "md" || s == "markdown" )
        return Language::markdown;
    return Language::none;
}

static bool is_identifier_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

static bool is_identifier_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

static bool is_number_start(char c, char next) {
    return std::isdigit(static_cast<unsigned char>(c)) || (c == '-' && std::isdigit(static_cast<unsigned char>(next)));
}

static bool is_number_char(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == 'x' || c == 'X' ||
           (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == 'e' || c == 'E' || c == '+' || c == '-';
}

std::vector<StyledSpan> SyntaxHighlighter::highlight(const std::string& line, Language lang) const {
    switch ( lang ) {
        case Language::json: return highlight_json(line);
        case Language::cpp: return highlight_cpp(line);
        case Language::javascript: return highlight_javascript(line);
        case Language::markdown: return highlight_markdown(line);
        default: return { StyledSpan{ line, 0, false } };
    }
}

std::vector<StyledSpan> SyntaxHighlighter::highlight_json(const std::string& line) const {
    std::vector<StyledSpan> spans;
    size_t i = 0;
    while ( i < line.size()) {
        char c = line[i];
        if ( std::isspace(static_cast<unsigned char>(c))) {
            size_t start = i;
            while ( i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
            spans.push_back({ line.substr(start, i - start), 0, false });
            continue;
        }
        if ( c == '"' ) {
            size_t start = i++;
            while ( i < line.size() && line[i] != '"' ) {
                if ( line[i] == '\\' && i + 1 < line.size()) i += 2;
                else i++;
            }
            if ( i < line.size()) i++;
            // crude key detection: string followed by ':'
            bool is_key = false;
            size_t j = i;
            while ( j < line.size() && std::isspace(static_cast<unsigned char>(line[j]))) j++;
            if ( j < line.size() && line[j] == ':' ) is_key = true;

            int color = is_key ? _type_pair : _string_pair;
            spans.push_back({ line.substr(start, i - start), color, false });
            continue;
        }
        if ( is_number_start(c, i + 1 < line.size() ? line[i + 1] : 0)) {
            size_t start = i;
            while ( i < line.size() && is_number_char(line[i])) i++;
            spans.push_back({ line.substr(start, i - start), _number_pair, false });
            continue;
        }
        if ( is_identifier_start(c)) {
            size_t start = i;
            while ( i < line.size() && is_identifier_char(line[i])) i++;
            std::string word = line.substr(start, i - start);
            bool lit = (word == "true" || word == "false" || word == "null");
            spans.push_back({ word, lit ? _keyword_pair : 0, false });
            continue;
        }
        spans.push_back({ std::string(1, c), 0, false });
        i++;
    }
    if ( spans.empty()) spans.push_back({ "", 0, false });
    return spans;
}

static const std::unordered_set<std::string> cpp_keywords = {
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
    "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
    "class", "compl", "concept", "const", "consteval", "constexpr", "constinit",
    "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
    "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit",
    "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline",
    "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
    "nullptr", "operator", "or", "or_eq", "private", "protected", "public",
    "register", "reinterpret_cast", "requires", "return", "short", "signed",
    "sizeof", "static", "static_assert", "static_cast", "struct", "switch",
    "template", "this", "thread_local", "throw", "true", "try", "typedef",
    "typeid", "typename", "union", "unsigned", "using", "virtual", "void",
    "volatile", "wchar_t", "while", "xor", "xor_eq", "include", "define",
    "ifdef", "ifndef", "endif", "elif", "pragma", "once"
};

std::vector<StyledSpan> SyntaxHighlighter::highlight_cpp(const std::string& line) const {
    std::vector<StyledSpan> spans;
    size_t i = 0;
    while ( i < line.size()) {
        char c = line[i];
        if ( c == ' ' || c == '\t' ) {
            size_t start = i;
            while ( i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
            spans.push_back({ line.substr(start, i - start), 0, false });
            continue;
        }
        if ( c == '/' && i + 1 < line.size() && line[i + 1] == '/' ) {
            spans.push_back({ line.substr(i), _comment_pair, false });
            break;
        }
        if ( c == '"' || c == '\'' ) {
            char quote = c;
            size_t start = i++;
            while ( i < line.size() && line[i] != quote ) {
                if ( line[i] == '\\' && i + 1 < line.size()) i += 2;
                else i++;
            }
            if ( i < line.size()) i++;
            spans.push_back({ line.substr(start, i - start), _string_pair, false });
            continue;
        }
        if ( is_number_start(c, i + 1 < line.size() ? line[i + 1] : 0)) {
            size_t start = i;
            while ( i < line.size() && is_number_char(line[i])) i++;
            spans.push_back({ line.substr(start, i - start), _number_pair, false });
            continue;
        }
        if ( is_identifier_start(c)) {
            size_t start = i;
            while ( i < line.size() && is_identifier_char(line[i])) i++;
            std::string word = line.substr(start, i - start);
            bool kw = cpp_keywords.count(word) > 0;
            spans.push_back({ word, kw ? _keyword_pair : 0, false });
            continue;
        }
        spans.push_back({ std::string(1, c), 0, false });
        i++;
    }
    if ( spans.empty()) spans.push_back({ "", 0, false });
    return spans;
}

static const std::unordered_set<std::string> js_keywords = {
    "break", "case", "catch", "class", "const", "continue", "debugger", "default",
    "delete", "do", "else", "export", "extends", "finally", "for", "function",
    "if", "import", "in", "instanceof", "let", "new", "return", "super", "switch",
    "this", "throw", "try", "typeof", "var", "void", "while", "with", "yield",
    "true", "false", "null", "undefined", "async", "await", "of", "static", "get",
    "set", "constructor"
};

std::vector<StyledSpan> SyntaxHighlighter::highlight_javascript(const std::string& line) const {
    std::vector<StyledSpan> spans;
    size_t i = 0;
    while ( i < line.size()) {
        char c = line[i];
        if ( c == ' ' || c == '\t' ) {
            size_t start = i;
            while ( i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
            spans.push_back({ line.substr(start, i - start), 0, false });
            continue;
        }
        if ( c == '/' && i + 1 < line.size() && line[i + 1] == '/' ) {
            spans.push_back({ line.substr(i), _comment_pair, false });
            break;
        }
        if ( c == '"' || c == '\'' || (c == '`' && i + 1 < line.size())) {
            char quote = c;
            size_t start = i++;
            while ( i < line.size() && line[i] != quote ) {
                if ( line[i] == '\\' && i + 1 < line.size()) i += 2;
                else i++;
            }
            if ( i < line.size()) i++;
            spans.push_back({ line.substr(start, i - start), _string_pair, false });
            continue;
        }
        if ( is_number_start(c, i + 1 < line.size() ? line[i + 1] : 0)) {
            size_t start = i;
            while ( i < line.size() && is_number_char(line[i])) i++;
            spans.push_back({ line.substr(start, i - start), _number_pair, false });
            continue;
        }
        if ( is_identifier_start(c)) {
            size_t start = i;
            while ( i < line.size() && is_identifier_char(line[i])) i++;
            std::string word = line.substr(start, i - start);
            bool kw = js_keywords.count(word) > 0;
            spans.push_back({ word, kw ? _keyword_pair : 0, false });
            continue;
        }
        spans.push_back({ std::string(1, c), 0, false });
        i++;
    }
    if ( spans.empty()) spans.push_back({ "", 0, false });
    return spans;
}

std::vector<StyledSpan> SyntaxHighlighter::highlight_markdown(const std::string& line) const {
    std::vector<StyledSpan> spans;
    size_t i = 0;

    auto is_space = [](char ch) {
        return std::isspace(static_cast<unsigned char>(ch));
    };

    auto is_word = [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
    };

    auto push_plain = [&](size_t start, size_t end) {
        if ( end > start )
            spans.push_back({ line.substr(start, end - start), 0, false, false });
    };

    auto push_link = [&](size_t start, size_t text_start, size_t text_end, size_t url_start, size_t url_end) {
        // Keep the complete Markdown spelling intact in the rendered text.
        // Splitting out only the label and URL used to drop `[`, `](` and `)`
        // from the live display, even though history replay showed the source.
        push_plain(start, text_start);
        if ( url_end + 1 > start )
            spans.push_back({ line.substr(text_start, url_end + 1 - text_start), 0, false, true });
    };

    while ( i < line.size()) {
        char c = line[i];

        // Block-style list markers at the start of a line or after a space.
        if ( i == 0 || line[i - 1] == ' ' ) {
            if ( ( c == '-' || c == '+' || c == '*' ) && i + 1 < line.size() && line[i + 1] == ' ' ) {
                spans.push_back({ line.substr(i, 2), _keyword_pair, true, false });
                i += 2;
                continue;
            }
            if ( std::isdigit(static_cast<unsigned char>(c)) ) {
                size_t j = i;
                while ( j < line.size() && std::isdigit(static_cast<unsigned char>(line[j])) ) ++j;
                if ( j + 1 < line.size() && line[j] == '.' && line[j + 1] == ' ' ) {
                    spans.push_back({ line.substr(i, j - i + 2), _keyword_pair, true, false });
                    i = j + 2;
                    continue;
                }
            }
        }

        // Task list checkbox: [ ] or [x]. Keep it visibly distinct but not "button-like".
        if ( c == '[' && i + 2 < line.size() && line[i + 2] == ']' ) {
            if ( line[i + 1] == ' ' || line[i + 1] == 'x' || line[i + 1] == 'X' ) {
                spans.push_back({ line.substr(i, 3), _keyword_pair, line[i + 1] != ' ', true });
                i += 3;
                continue;
            }
        }

        // Markdown links: make them look copyable, not clickable.
        if ( c == '[' ) {
            size_t text_start = i + 1;
            size_t close = line.find(']', text_start);
            if ( close != std::string::npos && close + 1 < line.size() && line[close + 1] == '(' ) {
                size_t url_start = close + 2;
                size_t url_end = line.find(')', url_start);
                if ( url_end != std::string::npos ) {
                    push_link(i, text_start, close, url_start, url_end);
                    i = url_end + 1;
                    continue;
                }
            }
        }

        if ( c == '#' ) {
            size_t start = i;
            while ( i < line.size() && line[i] == '#' ) i++;
            if ( i < line.size() && line[i] == ' ' ) {
                while ( i < line.size()) i++;
                spans.push_back({ line.substr(start), _keyword_pair, true, false });
                return spans;
            }
            spans.push_back({ line.substr(start, i - start), 0, false, false });
            continue;
        }
        if ( c == '`' ) {
            size_t start = i;
            int count = 0;
            while ( i < line.size() && line[i] == '`' ) { i++; count++; }
            size_t end = line.find(std::string(count, '`'), i);
            if ( end != std::string::npos ) {
                end += count;
                spans.push_back({ line.substr(start, end - start), _fence_pair, false, false });
                i = end;
                continue;
            }
            spans.push_back({ line.substr(start, count), _fence_pair, false, false });
            continue;
        }
        if ( c == '*' ) {
            size_t start = i;
            int count = 0;
            while ( i < line.size() && line[i] == '*' && count < 2 ) { i++; count++; }
            size_t end = line.find(std::string(count, '*'), i);
            if ( end != std::string::npos ) {
                end += count;
                spans.push_back({ line.substr(start, end - start), _string_pair, count == 2, false });
                i = end;
                continue;
            }
            spans.push_back({ line.substr(start, i - start), 0, false, false });
            continue;
        }
        if ( c == '_' ) {
            size_t start = i;
            int count = 0;
            while ( i < line.size() && line[i] == '_' && count < 2 ) { i++; count++; }
            bool open_ok = ( start == 0 || !is_word(line[start - 1])) &&
                           i < line.size() &&
                           !is_space(line[i]) && line[i] != '_';
            if ( open_ok ) {
                size_t end = line.find(std::string(count, '_'), i);
                while ( end != std::string::npos ) {
                    char after = ( end + count < line.size()) ? line[end + count] : '\0';
                    bool close_ok = !is_space(line[end - 1]) &&
                                    ( after == '\0' || !is_word(after));
                    if ( close_ok )
                        break;
                    end = line.find(std::string(count, '_'), end + 1);
                }
                if ( end != std::string::npos ) {
                    end += count;
                    spans.push_back({ line.substr(start, end - start), _string_pair, count == 2, false });
                    i = end;
                    continue;
                }
            }
            spans.push_back({ line.substr(start, count), 0, false, false });
            continue;
        }

        size_t start = i;
        while ( i < line.size() && line[i] != '#' && line[i] != '`' && line[i] != '*' && line[i] != '_' && line[i] != '[' )
            i++;
        push_plain(start, i);
    }
    if ( spans.empty()) spans.push_back({ "", 0, false, false });
    return spans;
}

} // namespace agent
