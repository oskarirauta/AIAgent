#pragma once

#include "agent/providers/openai.hpp"

namespace agent::providers {

class Moonshot : public OpenAI {
public:
    Moonshot(const Config& cfg) : OpenAI(cfg) {}

    std::string name() const override { return "moonshot"; }
    std::string endpoint() const override { return build_endpoint("/chat/completions"); }
};

} // namespace agent::providers
