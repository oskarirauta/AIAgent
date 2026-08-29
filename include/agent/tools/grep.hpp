#pragma once

#include "agent/tools/tool.hpp"

namespace agent::tools {

class Grep : public Tool {
public:
    std::string name() const override { return "grep"; }
    std::string description() const override {
        return "Search for a substring or regex in a text file or project subtree and "
               "return matching lines. Use it for narrow content search; use "
               "find_symbol/find_references/project_map for project orientation.";
    }
    JSON parameters() const override;
    std::string execute(const JSON& args) override;
};

} // namespace agent::tools
