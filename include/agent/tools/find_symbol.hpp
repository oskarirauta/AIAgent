#pragma once

#include "agent/tools/tool.hpp"

namespace agent::tools {

// Finds where a symbol (function, class, struct, type, …) is DEFINED across the
// project tree, so the model can navigate without knowing the file or crafting a
// tree-wide grep. Definition-aware and always fresh — no persistent index.
class FindSymbol : public Tool {
public:
    std::string name() const override { return "find_symbol"; }
    std::string description() const override;
    JSON parameters() const override;
    std::string execute(const JSON& args) override;
};

} // namespace agent::tools
