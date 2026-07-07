#pragma once

#include <functional>
#include <string>
#include <utility>
#include "agent/tools/tool.hpp"

namespace agent::tools {

// Lets the model load a skill (a named instruction set) by relevance. The tool's
// description enumerates the available skills (name + one-line description) so
// the model can pick one; execute() activates it and returns its instructions.
// Both callbacks are injected by the app (Repl), which owns the skill state.
class SkillTool : public Tool {
public:
    using describe_fn = std::function<std::string()>;               // full tool description
    using load_fn = std::function<std::string(const std::string&)>; // name -> instructions/result

    SkillTool(describe_fn describe, load_fn loader)
        : _describe(std::move(describe)), _loader(std::move(loader)) {}

    std::string name() const override { return "use_skill"; }
    std::string description() const override { return _describe ? _describe() : "Load a skill."; }

    JSON parameters() const override {
        return JSON::Object{
            { "type", "object" },
            { "properties", JSON::Object{
                { "name", JSON::Object{
                    { "type", "string" },
                    { "description", "the skill's name, from the list above" }
                }}
            }},
            { "required", JSON::Array{ "name" } }
        };
    }

    std::string execute(const JSON& args) override {
        std::string name;
        if ( args == JSON::TYPE::OBJECT && args.contains("name") && args["name"] == JSON::TYPE::STRING )
            name = args["name"].to_string();
        if ( name.empty())
            return "error: provide the `name` of a skill to load";
        if ( !_loader )
            return "error: skills are not available";
        return _loader(name);
    }

private:
    describe_fn _describe;
    load_fn _loader;
};

} // namespace agent::tools
