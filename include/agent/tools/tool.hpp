#pragma once

#include <string>
#include "json.hpp"

namespace agent::tools {

class Tool {
public:
    virtual ~Tool() = default;

    virtual std::string name() const = 0;
    virtual std::string description() const = 0;
    virtual JSON parameters() const = 0;
    virtual bool requires_confirmation() const { return false; }
    virtual std::string execute(const JSON& args) = 0;
};

} // namespace agent::tools
