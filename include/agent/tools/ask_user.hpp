#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "agent/tools/tool.hpp"
#include "json.hpp"

namespace agent::tools {

// Lets the model pause and ask the user a question — to resolve a choice or get
// missing information before doing a lot of work, instead of guessing. execute()
// presents the question (with optional multiple-choice options) and returns the
// user's answer as the tool result. The presentation callback is injected by the
// app (the REPL), which owns the terminal; it blocks until the user answers.
class AskUser : public Tool {
public:
    using ask_fn = std::function<std::string(const std::string& question,
                                             const std::vector<std::string>& options)>;

    explicit AskUser(ask_fn ask) : _ask(std::move(ask)) {}

    std::string name() const override { return "ask_user"; }
    std::string description() const override {
        return "Ask the user a question and wait for their answer. Use this when you need "
               "a decision or missing information before continuing (e.g. which approach to "
               "take, or a value only they know) — instead of guessing or stopping silently. "
               "Give `options` for a multiple-choice pick, or omit them for a free-text "
               "answer. Returns the user's answer.";
    }
    bool mutates() const override { return false; } // read-only; presents its own UI

    JSON parameters() const override {
        return JSON::Object{
            { "type", "object" },
            { "properties", JSON::Object{
                { "question", JSON::Object{
                    { "type", "string" },
                    { "description", "the question to ask the user" }
                }},
                { "options", JSON::Object{
                    { "type", "array" },
                    { "items", JSON::Object{ { "type", "string" } }},
                    { "description", "optional choices to pick from; omit for a free-text answer" }
                }}
            }},
            { "required", JSON::Array{ "question" } }
        };
    }

    std::string execute(const JSON& args) override {
        std::string q;
        if ( args == JSON::TYPE::OBJECT && args.contains("question") && args["question"] == JSON::TYPE::STRING )
            q = args["question"].to_string();
        if ( q.empty())
            return "error: provide a `question`";
        std::vector<std::string> opts;
        if ( args == JSON::TYPE::OBJECT && args.contains("options") && args["options"] == JSON::TYPE::ARRAY )
            for ( size_t i = 0; i < args["options"].size(); ++i )
                if ( args["options"][i] == JSON::TYPE::STRING )
                    opts.push_back(args["options"][i].to_string());
        if ( !_ask )
            return "no interactive terminal available to ask the user";
        std::string answer = _ask(q, opts);
        if ( answer.empty())
            return "the user did not answer";
        return "the user answered: " + answer;
    }

private:
    ask_fn _ask;
};

} // namespace agent::tools
