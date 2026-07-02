#pragma once

#include "agent/tools/tool.hpp"

namespace agent::tools {

class ListDirectory : public Tool {
public:
    std::string name() const override { return "list_directory"; }
    std::string description() const override { return "List files and directories at the given path."; }
    JSON parameters() const override;
    std::string execute(const JSON& args) override;
};

} // namespace agent::tools
