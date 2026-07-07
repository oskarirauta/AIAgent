#pragma once

#include <memory>
#include <utility>
#include "agent/tools/tool.hpp"
#include "agent/tools/file_tracker.hpp"

namespace agent::tools {

class ReadFile : public Tool {
public:
    explicit ReadFile(std::shared_ptr<FileTracker> tracker = nullptr) : _tracker(std::move(tracker)) {}
    std::string name() const override { return "read_file"; }
    std::string description() const override { return "Read the full contents of a text file."; }
    JSON parameters() const override;
    std::string execute(const JSON& args) override;

private:
    std::shared_ptr<FileTracker> _tracker;
};

} // namespace agent::tools
