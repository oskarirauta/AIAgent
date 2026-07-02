#include "agent/tools/registry.hpp"

#include "throws.hpp"
#include "logger.hpp"
#include "agent/tools/read_file.hpp"
#include "agent/tools/write_file.hpp"
#include "agent/tools/run_command.hpp"
#include "agent/tools/list_directory.hpp"
#include "agent/tools/grep.hpp"

namespace agent::tools {

void Registry::register_defaults() {
    add(std::make_unique<ReadFile>());
    add(std::make_unique<WriteFile>());
    add(std::make_unique<RunCommand>());
    add(std::make_unique<ListDirectory>());
    add(std::make_unique<Grep>());
}

void Registry::add(std::unique_ptr<Tool> tool) {
    _tools[tool->name()] = std::move(tool);
}

void Registry::set_confirm_callback(confirm_cb_t cb) {
    _confirm_cb = std::move(cb);
}

JSON Registry::schema() const {
    JSON arr = JSON::Array{};
    for ( const auto& [name, tool] : _tools ) {
        JSON entry = JSON::Object{
            { "type", "function" },
            { "function", JSON::Object{
                { "name", tool->name() },
                { "description", tool->description() },
                { "parameters", tool->parameters() }
            }}
        };
        arr.append(entry);
    }
    return arr;
}

std::string Registry::execute(const std::string& name, const JSON& args) {
    auto it = _tools.find(name);
    if ( it == _tools.end())
        throws << "unknown tool: " << name << std::endl;

    if ( it->second->requires_confirmation()) {
        std::string action = name + " " + args.dump_minified();
        if ( _confirm_cb && !_confirm_cb(action)) {
            logger::info["tool"] << "user declined " << name << std::endl;
            return "user declined to run " + name;
        }
    }

    logger::info["tool"] << "executing " << name << std::endl;
    return it->second->execute(args);
}

bool Registry::has(const std::string& name) const {
    return _tools.find(name) != _tools.end();
}

} // namespace agent::tools
