#pragma once

#include <string>
#include <vector>
#include "agent/version.hpp" // agent::VERSION (kept available via this header too)

namespace agent {

// One slash command, the single source of truth for /help, /help <cmd> and the
// generated COMMANDS.md.
struct CommandDoc {
    std::string name;     // primary name, e.g. "/model"
    std::string aliases;  // comma-separated, e.g. "/info" (empty if none)
    std::string usage;    // argument spec, e.g. "[name]" (empty if none)
    std::string group;    // section heading
    std::string summary;  // one line
    std::string detail;   // longer help shown by /help <cmd> (may be multi-line)
};

// The full command catalogue, grouped (stable order).
const std::vector<CommandDoc>& command_catalog();

// /help with no argument: the grouped one-line list.
std::string commands_overview();

// /help <query>: detailed help for one command (name or alias, with or without
// a leading '/'). Empty result string means "not found".
std::string command_help(const std::string& query);

// The catalogue rendered as COMMANDS.md (used to (re)generate the doc).
std::string commands_markdown();

} // namespace agent
