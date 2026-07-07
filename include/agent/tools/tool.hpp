#pragma once

#include <string>
#include "json.hpp"

namespace agent::tools {

class Tool {
public:
    virtual ~Tool() = default;

    virtual std::string name() const = 0;
    virtual std::string description() const = 0;
    virtual JSON parameters() const = 0;
    virtual bool requires_confirmation() const { return false; }
    // A non-empty reason marks this call dangerous (always acknowledged, even in
    // automatic mode) — e.g. an MCP tool whose server sets destructiveHint.
    virtual std::string danger_reason(const JSON& args) const { (void)args; return ""; }
    // Whether the tool can modify files or run commands. Read-only tools return
    // false and stay available in plan mode; mutating tools are blocked there.
    virtual bool mutates() const { return false; }
    virtual std::string execute(const JSON& args) = 0;
};

} // namespace agent::tools
