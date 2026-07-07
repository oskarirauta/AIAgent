#pragma once

#include <functional>
#include <string>
#include <utility>
#include "agent/tools/tool.hpp"
#include "json.hpp"

namespace agent::tools {

// Inspect (or stop) a background job started with run_command(background:true).
// The callback is injected by the app, which owns the job manager.
class CheckJob : public Tool {
public:
    // (id, stop) -> a human/model-readable status + recent output.
    using check_fn = std::function<std::string(int id, bool stop)>;

    explicit CheckJob(check_fn check) : _check(std::move(check)) {}

    std::string name() const override { return "check_job"; }
    bool mutates() const override { return false; }
    std::string description() const override {
        return "Check a background job started with run_command(background:true): returns whether "
               "it is still running and its recent output. Pass stop:true to stop the job. Use this "
               "to see if a dev server came up, read a watcher's log, or shut a job down when done.";
    }
    JSON parameters() const override {
        return JSON::Object{
            { "type", "object" },
            { "properties", JSON::Object{
                { "id", JSON::Object{
                    { "type", "integer" },
                    { "description", "the background job id (from run_command's reply or /jobs)" }
                }},
                { "stop", JSON::Object{
                    { "type", "boolean" },
                    { "description", "stop the job instead of just reading it" }
                }}
            }},
            { "required", JSON::Array{ "id" } }
        };
    }

    std::string execute(const JSON& args) override {
        if ( args != JSON::TYPE::OBJECT || !args.contains("id"))
            return "error: provide the job `id`";
        int id = 0;
        if ( args["id"] == JSON::TYPE::INT ) id = static_cast<int>(static_cast<long long>(args["id"]));
        else return "error: `id` must be an integer";
        bool stop = args.contains("stop") && args["stop"] == JSON::TYPE::BOOL && static_cast<bool>(args["stop"]);
        if ( !_check )
            return "background jobs are not available in this mode";
        return _check(id, stop);
    }

private:
    check_fn _check;
};

} // namespace agent::tools
