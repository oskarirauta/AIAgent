#pragma once

#include <map>
#include <memory>
#include <string>
#include "agent/tools/tool.hpp"

namespace agent::tools {

class Registry {
public:
    void register_defaults();
    void add(std::unique_ptr<Tool> tool);

    JSON schema() const;
    std::string execute(const std::string& name, const JSON& args);
    bool has(const std::string& name) const;

private:
    std::map<std::string, std::unique_ptr<Tool>> _tools;
};

} // namespace agent::tools
