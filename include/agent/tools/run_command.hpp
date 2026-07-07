#pragma once

#include <functional>
#include <string>
#include "agent/tools/tool.hpp"

namespace agent::tools {

class RunCommand : public Tool {
public:
    // Start a fully-built shell command as a background job, returning a short
    // note (job id). Injected by the app; when unset, `background:true` runs
    // synchronously as usual. Lets the model launch a dev server / watcher.
    using background_fn = std::function<std::string(const std::string& shell_command)>;
    void set_background(background_fn fn) { _background = std::move(fn); }

    std::string name() const override { return "run_command"; }
    bool mutates() const override { return true; }
    std::string description() const override { return "Run a shell command and return its stdout and stderr."; }
    JSON parameters() const override;
    bool requires_confirmation() const override { return true; }
    std::string execute(const JSON& args) override;

private:
    background_fn _background;
};

} // namespace agent::tools
