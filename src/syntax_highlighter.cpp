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
    while ( i < line.size()) {
        char c = line[i];
        if ( c == '#' ) {
            size_t start = i;
            while ( i < line.size() && line[i] == '#' ) i++;
            if ( i < line.size() && line[i] == ' ' ) {
                while ( i < line.size()) i++;
                spans.push_back({ line.substr(start), _keyword_pair, true });
                return spans;
            }
            spans.push_back({ line.substr(start, i - start), 0, false });
            continue;
        }
        if ( c == '`' ) {
            size_t start = i;
            while ( i < line.size() && line[i] == '`' ) i++;
            spans.push_back({ line.substr(start, i - start), _fence_pair, false });
            continue;
        }
        if ( c == '*' || c == '_' ) {
            char mark = c;
            size_t start = i;
            int count = 0;
            while ( i < line.size() && line[i] == mark && count < 2 ) { i++; count++; }
            size_t end = line.find(std::string(count, mark), i);
            if ( end != std::string::npos ) {
                end += count;
                spans.push_back({ line.substr(start, end - start), _string_pair, count == 2 });
                i = end;
                continue;
            }
            spans.push_back({ line.substr(start, i - start), 0, false });
            continue;
        }
        size_t start = i;
        while ( i < line.size() && line[i] != '#' && line[i] != '`' && line[i] != '*' && line[i] != '_' )
            i++;
        spans.push_back({ line.substr(start, i - start), 0, false });
    }
    if ( spans.empty()) spans.push_back({ "", 0, false });
    return spans;
}

} // namespace agent
