#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include "agent/tools/tool.hpp"

namespace agent::tools {

class Registry {
public:
    using confirm_cb_t = std::function<bool(const std::string& action)>;

    void register_defaults();
    void add(std::unique_ptr<Tool> tool);
    void set_confirm_callback(confirm_cb_t cb);

    JSON schema() const;
    std::string execute(const std::string& name, const JSON& args);
    bool has(const std::string& name) const;

private:
    std::map<std::string, std::unique_ptr<Tool>> _tools;
    confirm_cb_t _confirm_cb;
};

} // namespace agent::tools
