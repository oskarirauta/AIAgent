#pragma once

#include <memory>
#include <utility>
#include "agent/tools/tool.hpp"
#include "agent/tools/file_tracker.hpp"

namespace agent::tools {

class WriteFile : public Tool {
public:
    explicit WriteFile(std::shared_ptr<FileTracker> tracker = nullptr) : _tracker(std::move(tracker)) {}
    std::string name() const override { return "write_file"; }
    bool mutates() const override { return true; }
    std::string description() const override { return "Write text content to a file, overwriting if it exists."; }
    JSON parameters() const override;
    bool requires_confirmation() const override { return true; }
    std::string execute(const JSON& args) override;

private:
    std::shared_ptr<FileTracker> _tracker;
};

} // namespace agent::tools
