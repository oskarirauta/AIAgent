#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "agent/tools/tool.hpp"

namespace agent::tools {

// Lets the model launch a background workflow: a named list of independent steps,
// each run by its own read-only sub-agent. The tool returns immediately with a
// run id; the user watches progress with /workflows. The actual launch is done
// by the app (Repl) via an injected handler.
class WorkflowTool : public Tool {
public:
    // (name, steps) -> message shown to the model (e.g. "started workflow #3").
    using launch_fn = std::function<std::string(const std::string& name,
                                                const std::vector<std::string>& steps)>;

    explicit WorkflowTool(launch_fn launcher) : _launcher(std::move(launcher)) {}

    std::string name() const override { return "run_workflow"; }

    std::string description() const override {
        return "Launch a background workflow to take on a larger task. Provide a short "
               "`name` and a list of `steps`, where each step is a self-contained task "
               "for an independent read-only sub-agent (it can read files, list "
               "directories and grep, but not write or run commands). Steps run in the "
               "background and in order; the tool returns immediately with a run id. The "
               "user watches progress with /workflows, and the results are handed back to "
               "you on your next turn. Use this to fan out research or analysis across a "
               "codebase; keep each step focused and self-describing.";
    }

    JSON parameters() const override {
        return JSON::Object{
            { "type", "object" },
            { "properties", JSON::Object{
                { "name", JSON::Object{
                    { "type", "string" },
                    { "description", "A short label for the workflow run." }
                }},
                { "steps", JSON::Object{
                    { "type", "array" },
                    { "items", JSON::Object{ { "type", "string" } } },
                    { "description", "Ordered list of self-contained tasks, one per sub-agent." }
                }}
            }},
            { "required", JSON::Array{ "steps" } }
        };
    }

    std::string execute(const JSON& args) override {
        if ( args != JSON::TYPE::OBJECT )
            return "error: invalid arguments";
        std::string name;
        if ( args.contains("name") && args["name"] == JSON::TYPE::STRING )
            name = args["name"].to_string();

        std::vector<std::string> steps;
        if ( args.contains("steps") && args["steps"] == JSON::TYPE::ARRAY ) {
            const JSON& arr = args["steps"];
            for ( size_t i = 0; i < arr.size(); ++i )
                if ( arr[i] == JSON::TYPE::STRING && !arr[i].to_string().empty())
                    steps.push_back(arr[i].to_string());
        }
        if ( steps.empty())
            return "error: provide a non-empty `steps` array of task strings";
        if ( !_launcher )
            return "error: workflows are not available";
        return _launcher(name, steps);
    }

private:
    launch_fn _launcher;
};

} // namespace agent::tools
