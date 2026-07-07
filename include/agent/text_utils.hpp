#pragma once

#include <string>

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

} // namespace agent
