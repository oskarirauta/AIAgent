#include "agent/providers/provider.hpp"

#include "agent/providers/openai.hpp"
#include "agent/providers/ollama.hpp"
#include "agent/providers/anthropic.hpp"
#include "agent/providers/moonshot.hpp"
#include "throws.hpp"

namespace agent::providers {

std::unique_ptr<Provider> create(const Config& cfg) {
    if ( cfg.provider == "openai" )
        return std::make_unique<OpenAI>(cfg);
    if ( cfg.provider == "ollama" )
        return std::make_unique<Ollama>(cfg);
    if ( cfg.provider == "anthropic" )
        return std::make_unique<Anthropic>(cfg);
    if ( cfg.provider == "moonshot" )
        return std::make_unique<Moonshot>(cfg);

    throws << "unsupported provider: " << cfg.provider << std::endl;
    return nullptr;
}

} // namespace agent::providers
