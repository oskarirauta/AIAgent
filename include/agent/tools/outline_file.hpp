#pragma once

#include "agent/tools/tool.hpp"

namespace agent::tools {

// Returns a "map" of one file — its definition lines (functions, classes,
// structs, enums, types, …) with line numbers, without the bodies. Lets the
// model navigate a large file cheaply: outline first, then read_file at the
// right offset. The missing fourth member of the project_map / find_symbol /
// find_references family (project-wide) applied to a single file.
class OutlineFile : public Tool {
public:
    std::string name() const override { return "outline_file"; }
    std::string description() const override {
        return "Outline one file: list its definitions (functions, classes, structs, "
               "enums, types, …) as line-number: signature, without the bodies. Use it to "
               "navigate a large file cheaply — get the outline, then read_file at the "
               "line you need — instead of reading the whole file.";
    }
    JSON parameters() const override;
    std::string execute(const JSON& args) override;
};

} // namespace agent::tools
