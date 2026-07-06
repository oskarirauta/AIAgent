#pragma once

#include <functional>
#include <string>
#include <utility>
#include "agent/tools/tool.hpp"

namespace agent::tools {

// Lets the main model consult a stronger "advisor" model on a hard problem (e.g.
// claude-sonnet asking claude-opus for a second opinion). The actual LLM call is
// supplied by the app (Repl) via a handler, so this tool stays free of any
// provider/client dependency.
class AdvisorTool : public Tool {
public:
    using handler_t = std::function<std::string(const std::string& question)>;

    explicit AdvisorTool(handler_t handler) : _handler(std::move(handler)) {}

    std::string name() const override { return "consult_advisor"; }

    std::string description() const override {
        return "Consult a more capable advisor model for a second opinion on a hard "
               "problem. Use it when you are stuck, unsure of the right approach, or want "
               "a design or logic review. The advisor cannot see the repository or run "
               "tools, so put everything it needs — the goal, constraints, relevant code "
               "and what you have tried — into `question`.";
    }

    JSON parameters() const override {
        return JSON::Object{
            { "type", "object" },
            { "properties", JSON::Object{
                { "question", JSON::Object{
                    { "type", "string" },
                    { "description", "The problem to ask the advisor, with enough self-contained context to answer." }
                }}
            }},
            { "required", JSON::Array{ "question" } }
        };
    }

    std::string execute(const JSON& args) override {
        std::string question;
        if ( args == JSON::TYPE::OBJECT && args.contains("question") &&
             args["question"] == JSON::TYPE::STRING )
            question = args["question"].to_string();
        if ( question.empty())
            return "error: no question provided";
        if ( !_handler )
            return "error: advisor is not available";
        return _handler(question);
    }

private:
    handler_t _handler;
};

} // namespace agent::tools
