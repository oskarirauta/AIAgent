#pragma once

#include <string>
#include <vector>

namespace agent {

enum class Language {
    none,
    json,
    cpp,
    javascript,
    markdown
};

struct StyledSpan {
    std::string text;
    int color_pair = 0; // 0 = default
    bool bold = false;
};

class SyntaxHighlighter {
public:
    explicit SyntaxHighlighter(int base_color_pairs);

    Language detect(const std::string& fence) const;
    std::vector<StyledSpan> highlight(const std::string& line, Language lang) const;

    int color_for_keyword() const { return _keyword_pair; }
    int color_for_string() const { return _string_pair; }
    int color_for_comment() const { return _comment_pair; }
    int color_for_number() const { return _number_pair; }
    int color_for_type() const { return _type_pair; }
    int color_for_fence() const { return _fence_pair; }

private:
    int _keyword_pair;
    int _string_pair;
    int _comment_pair;
    int _number_pair;
    int _type_pair;
    int _fence_pair;

    std::vector<StyledSpan> highlight_json(const std::string& line) const;
    std::vector<StyledSpan> highlight_cpp(const std::string& line) const;
    std::vector<StyledSpan> highlight_javascript(const std::string& line) const;
    std::vector<StyledSpan> highlight_markdown(const std::string& line) const;
};

} // namespace agent
