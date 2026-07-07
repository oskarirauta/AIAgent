#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_set>
#include "json.hpp"
#include "agent/conversation.hpp"
#include "agent/config.hpp"
#include "agent/api/client.hpp"

namespace agent::providers {

struct ToolCall {
    std::string id;
    std::string name;
    JSON arguments;
};

struct Response {
    std::string message;
    std::string thinking;    // reasoning/thinking content, if the model returned any
    std::vector<ToolCall> tool_calls;
    bool success = true;
    long input_tokens = 0;   // prompt/context tokens reported by the provider (0 if unknown)
    long output_tokens = 0;  // generated tokens reported by the provider (0 if unknown)

    // Raw Anthropic thinking/redacted_thinking blocks (with signatures), verbatim.
    // Replayed with the assistant turn when extended thinking + tool use.
    JSON thinking_blocks = JSON::Array{};
};

// Visible deltas produced by one streamed chunk. Content and reasoning are
// accumulated internally by the provider too (see stream_result()).
struct StreamChunk {
    std::string content;   // answer text delta
    std::string reasoning; // thinking/reasoning delta
};

class Provider {
public:
    virtual ~Provider() = default;

    virtual std::string name() const = 0;
    virtual std::string endpoint() const = 0;
    virtual std::string auth_header() const { return "Authorization"; }
    virtual std::string auth_value() const { return _config.api_key.empty() ? "" : "Bearer " + _config.api_key; }
    virtual void prepare_request(api::Client& client) { (void)client; }
    virtual bool authenticate(api::Client& client, bool force = false) { (void)client; (void)force; return true; }

    // Non-interactive readiness check for a mid-session /provider switch: refresh
    // an existing token if it is close to expiry, but NEVER fall back to an
    // interactive login (the raw-mode REPL cannot host one). Returns true when the
    // provider can be used right away, false when a fresh login is required.
    // Providers without authentication are always ready.
    virtual bool ready_noninteractive(api::Client& client) { (void)client; return true; }

    virtual bool supports_streaming() const { return false; }

    // Streaming: reset per-turn accumulation, parse one SSE chunk (returning the
    // visible deltas while accumulating content/reasoning/tool_calls internally),
    // and assemble the full Response once the stream is done. Providers that can
    // stream tool calls override all three; others only produce content.
    virtual void stream_reset() {}
    virtual StreamChunk parse_stream(const std::string& chunk, std::string& buffer, bool& done) { (void)chunk; (void)buffer; (void)done; return {}; }
    virtual Response stream_result() { return Response{}; }

    virtual JSON build_request(const Conversation& conv, const JSON& tools_schema) = 0;
    virtual Response parse_response(const JSON& response) = 0;
    virtual JSON make_tool_result(const std::string& tool_call_id, const std::string& result) = 0;

    // Provider-specific capabilities (e.g. "model-command", "image-input").
    // Return an empty set by default; override in subclasses that support extras.
    virtual std::unordered_set<std::string> capabilities() const { return {}; }
    bool supports(const std::string& capability) const {
        return capabilities().find(capability) != capabilities().end();
    }

    // Apply provider-specific options loaded from config (e.g. provider.kimi.model).
    // Override in subclasses that expose custom settings.
    virtual void apply_provider_options(const JSON& options) { (void)options; }

    // Change the active model at runtime (used by the /model slash command).
    void set_model(const std::string& model) { _config.model = model; }
    const std::string& model() const { return _config.model; }

    // Extra HTTP headers to add to every request. Override in subclasses that need
    // provider-specific headers (e.g. Anthropic's anthropic-version).
    virtual std::vector<std::pair<std::string, std::string>> extra_headers() const { return {}; }

    const Config& config() const { return _config; }

protected:
    Provider(const Config& cfg) : _config(cfg) {}
    Config _config;

    // The messages to send, trimmed to the configured context budget (if any).
    // build_request implementations iterate this instead of conv.messages() so
    // history that would overflow a small context window (e.g. local models) is
    // dropped from the request while the full history stays saved.
    std::vector<Message> request_messages(const Conversation& conv) const {
        return conv.within_token_budget(_config.context_budget());
    }

    std::string build_endpoint(const std::string& path) const {
        std::string url = _config.api_url;
        while (!url.empty() && url.back() == '/')
            url.pop_back();
        if (url.size() >= path.size() && url.compare(url.size() - path.size(), path.size(), path) == 0)
            return url;
        return url + path;
    }
};

std::unique_ptr<Provider> create(const Config& cfg);

} // namespace agent::providers
