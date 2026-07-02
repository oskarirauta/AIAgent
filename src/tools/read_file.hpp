#pragma once

#include "tool.hpp"

namespace agent::tools {

class ReadFile : public Tool {
public:
    std::string name() const override { return "read_file"; }
    std::string description() const override { return "Read the full contents of a text file."; }
    JSON parameters() const override;
    std::string execute(const JSON& args) override;
};

} // namespace agent::tools
