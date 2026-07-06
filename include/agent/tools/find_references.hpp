#pragma once

#include "agent/tools/tool.hpp"

namespace agent::tools {

// Finds where a symbol is USED (call sites / references) across the project tree,
// as whole-identifier matches — the usage counterpart to find_symbol (which finds
// definitions). More precise than grep, which matches substrings.
class FindReferences : public Tool {
public:
    std::string name() const override { return "find_references"; }
    std::string description() const override;
    JSON parameters() const override;
    std::string execute(const JSON& args) override;
};

} // namespace agent::tools
