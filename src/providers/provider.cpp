#include "agent/providers/provider.hpp"

#include "agent/providers/openai.hpp"
#include "agent/providers/ollama.hpp"
#include "agent/providers/anthropic.hpp"
#include "agent/providers/moonshot.hpp"
#include "agent/providers/openrouter.hpp"
#include "agent/providers/kimi.hpp"
#include "agent/providers/claude.hpp"
#include "throws.hpp"

namespace agent::providers {

std::unique_ptr<Provider> create(const Config& cfg) {
    std::unique_ptr<Provider> provider;
    if ( cfg.provider == "openai" )
        provider = std::make_unique<OpenAI>(cfg);
    else if ( cfg.provider == "ollama" )
        provider = std::make_unique<Ollama>(cfg);
    else if ( cfg.provider == "anthropic" )
        provider = std::make_unique<Anthropic>(cfg);
    else if ( cfg.provider == "moonshot" )
        provider = std::make_unique<Moonshot>(cfg);
    else if ( cfg.provider == "openrouter" )
        provider = std::make_unique<OpenRouter>(cfg);
    else if ( cfg.provider == "kimi" )
        provider = std::make_unique<Kimi>(cfg);
    else if ( cfg.provider == "claude" )
        provider = std::make_unique<Claude>(cfg);
    else {
        throws << "unsupported provider: " << cfg.provider << std::endl;
        return nullptr;
    }

    auto it = cfg.provider_options.find(cfg.provider);
    if ( it != cfg.provider_options.end())
        provider->apply_provider_options(it->second);

    return provider;
}

} // namespace agent::providers
