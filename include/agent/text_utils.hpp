#pragma once

#include <string>

namespace agent {

// Replace common UTF-8 punctuation and symbols with ASCII equivalents.
// Useful for models and terminals that have trouble with non-ASCII characters.
std::string normalize_text(std::string s);

} // namespace agent
