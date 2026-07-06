#pragma once

#include "agent/providers/openai.hpp"

namespace agent::providers {

// OpenRouter (https://openrouter.ai) is an OpenAI-compatible gateway to many
// models (openai/…, anthropic/…, deepseek/…, meta-llama/…, plus free `:free`
// variants). It's a thin OpenAI subclass: same chat-completions API and Bearer
// auth, with OpenRouter's endpoint and optional attribution headers.
class OpenRouter : public OpenAI {
public:
    OpenRouter(const Config& cfg) : OpenAI(cfg) {
        if ( _config.api_url == Config().api_url )
            _config.api_url = "https://openrouter.ai/api/v1";
    }

    std::string name() const override { return "openrouter"; }
    std::string endpoint() const override { return build_endpoint("/chat/completions"); }

    // Optional OpenRouter attribution headers (not required, harmless if kept).
    std::vector<std::pair<std::string, std::string>> extra_headers() const override {
        return {
            { "HTTP-Referer", "https://github.com/oskarirauta/AIAgent" },
            { "X-Title", "AIAgent" }
        };
    }
};

} // namespace agent::providers
