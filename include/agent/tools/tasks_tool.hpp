#pragma once

#include <functional>
#include <string>
#include <utility>
#include "agent/tools/tool.hpp"

namespace agent::tools {

// Lets the model keep a short todo list for the current task so multi-step work
// stays on track. The list lives in the app (Repl); this tool forwards the full
// task array to an injected handler.
class TasksTool : public Tool {
public:
    using handler_t = std::function<std::string(const JSON& tasks)>;

    explicit TasksTool(handler_t handler) : _handler(std::move(handler)) {}

    std::string name() const override { return "update_tasks"; }

    std::string description() const override {
        return "Maintain a short todo list for the current multi-step task so nothing is "
               "forgotten. Call this with the FULL list each time — it replaces the "
               "previous one. Mark each item done as you finish it and set the next to "
               "in_progress. Keep exactly one item in_progress at a time. The user sees "
               "the list with /tasks. Use it for non-trivial work; skip it for one-shot "
               "answers.";
    }

    JSON parameters() const override {
        return JSON::Object{
            { "type", "object" },
            { "properties", JSON::Object{
                { "tasks", JSON::Object{
                    { "type", "array" },
                    { "items", JSON::Object{
                        { "type", "object" },
                        { "properties", JSON::Object{
                            { "title", JSON::Object{ { "type", "string" } } },
                            { "status", JSON::Object{
                                { "type", "string" },
                                { "enum", JSON::Array{ "pending", "in_progress", "done" } } } }
                        }},
                        { "required", JSON::Array{ "title", "status" } }
                    }},
                    { "description", "the full task list (replaces the previous one)" }
                }}
            }},
            { "required", JSON::Array{ "tasks" } }
        };
    }

    std::string execute(const JSON& args) override {
        if ( args != JSON::TYPE::OBJECT || !args.contains("tasks") || args["tasks"] != JSON::TYPE::ARRAY )
            return "error: provide a `tasks` array of { title, status }";
        if ( !_handler )
            return "error: tasks are not available";
        return _handler(args["tasks"]);
    }

private:
    handler_t _handler;
};

} // namespace agent::tools
