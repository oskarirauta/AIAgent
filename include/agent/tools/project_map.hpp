#pragma once

#include "agent/tools/tool.hpp"

namespace agent::tools {

// Gives the model a quick overview of a project: which build/manifest files are
// present (parsed for name, scripts/targets and dependency counts), the top-level
// directory layout with file counts, and a language histogram. Cheaper than
// exploring the tree with many read/list calls.
class ProjectMap : public Tool {
public:
    std::string name() const override { return "project_map"; }
    std::string description() const override {
        return "Get a high-level map of the project: the build/manifest files present "
               "(name, scripts/targets, dependency counts), the top-level directory layout "
               "with file counts, and a source-language histogram. Call it once at the "
               "start of a task to orient yourself instead of many list/read calls.";
    }
    JSON parameters() const override;
    std::string execute(const JSON& args) override;
};

} // namespace agent::tools
