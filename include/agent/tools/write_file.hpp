#pragma once

#include "agent/tools/tool.hpp"

namespace agent::tools {

class WriteFile : public Tool {
public:
    std::string name() const override { return "write_file"; }
    std::string description() const override { return "Write text content to a file, overwriting if it exists."; }
    JSON parameters() const override;
    std::string execute(const JSON& args) override;
};

} // namespace agent::tools
