#pragma once

#include "tool.hpp"

namespace agent::tools {

class Grep : public Tool {
public:
    std::string name() const override { return "grep"; }
    std::string description() const override { return "Search for a substring pattern in a text file and return matching lines."; }
    JSON parameters() const override;
    std::string execute(const JSON& args) override;
};

} // namespace agent::tools
