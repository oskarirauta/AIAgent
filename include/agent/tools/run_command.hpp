#pragma once

#include "agent/tools/tool.hpp"

namespace agent::tools {

class RunCommand : public Tool {
public:
    std::string name() const override { return "run_command"; }
    bool mutates() const override { return true; }
    std::string description() const override { return "Run a shell command and return its stdout and stderr."; }
    JSON parameters() const override;
    bool requires_confirmation() const override { return true; }
    std::string execute(const JSON& args) override;
};

} // namespace agent::tools
