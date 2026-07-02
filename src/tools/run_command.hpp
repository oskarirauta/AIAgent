#pragma once

#include "tool.hpp"

namespace agent::tools {

class RunCommand : public Tool {
public:
    std::string name() const override { return "run_command"; }
    std::string description() const override { return "Run a shell command and return its stdout and stderr."; }
    JSON parameters() const override;
    std::string execute(const JSON& args) override;
};

} // namespace agent::tools
