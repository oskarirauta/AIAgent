#pragma once

#include <string>
#include <vector>

namespace agent {

// Replace common UTF-8 punctuation and symbols with ASCII equivalents.
// Useful for models and terminals that have trouble with non-ASCII characters.
std::string normalize_text(std::string s);

// A compact line-based diff: trims the common leading/trailing lines and shows
// the changed middle as -old / +new with a little surrounding context. Capped.
std::string block_diff(const std::string& old_text, const std::string& new_text,
                       const std::string& from_label = "before",
                       const std::string& to_label = "after");

// Whole-word, case-insensitive test for the "ultracode"/"ultrathink" markers,
// which raise the Anthropic thinking effort to max for a single turn.
bool has_ultra_keyword(const std::string& s);

// One expanded @path file mention (see expand_file_mentions).
struct FileMention {
    std::string path;
    size_t lines = 0;
    bool truncated = false;
};

// Expand whitespace-delimited "@path" tokens into inline file blocks when the
// path names an existing regular file (with per-file and total size caps).
// A token only counts when the '@' starts it (so a@b.c is untouched) and the
// path exists (so @decorator is untouched). Trailing punctuation is tolerated:
// "(see @src/x.cpp)" resolves src/x.cpp.
std::string expand_file_mentions(const std::string& text,
                                 std::vector<FileMention>* mentions = nullptr);

} // namespace agent
