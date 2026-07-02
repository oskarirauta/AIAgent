#pragma once

#include "agent/providers/openai.hpp"

namespace agent::providers {

class Moonshot : public OpenAI {
public:
    Moonshot(const Config& cfg) : OpenAI(cfg) {}

    std::string name() const override { return "moonshot"; }
    std::string endpoint() const override { return _config.api_url; }
};

} // namespace agent::providers
