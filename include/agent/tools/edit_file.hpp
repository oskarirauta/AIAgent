#pragma once

#include "agent/tools/tool.hpp"

namespace agent::tools {

// Targeted edit: replace an exact snippet in an existing file, instead of
// rewriting the whole thing (cheaper, safer, and it plays with /changes).
class EditFile : public Tool {
public:
    std::string name() const override { return "edit_file"; }
    std::string description() const override {
        return "Make a targeted edit to an existing file by replacing an exact snippet. "
               "Provide `old_string` (the exact text to replace, with enough surrounding "
               "context to be unique) and `new_string`. `old_string` must match exactly, "
               "including whitespace and indentation, and appear exactly once unless "
               "`replace_all` is true. Prefer this over write_file for edits. The file must "
               "already exist (use write_file to create a new one). To make several changes "
               "at once, pass an `edits` array — they apply in order, atomically (if any fails "
               "the file is left unchanged).";
    }
    JSON parameters() const override;
    bool requires_confirmation() const override { return true; }
    std::string execute(const JSON& args) override;
};

} // namespace agent::tools
