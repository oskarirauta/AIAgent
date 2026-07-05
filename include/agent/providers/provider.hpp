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
    std::vector<ToolCall> tool_calls;
    bool success = true;
    long input_tokens = 0;   // prompt/context tokens reported by the provider (0 if unknown)
    long output_tokens = 0;  // generated tokens reported by the provider (0 if unknown)
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
    virtual bool supports_streaming() const { return false; }
    virtual std::string parse_stream(const std::string& chunk, std::string& buffer, bool& done) { return ""; }
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

    // Extra HTTP headers to add to every request. Override in subclasses that need
    // provider-specific headers (e.g. Anthropic's anthropic-version).
    virtual std::vector<std::pair<std::string, std::string>> extra_headers() const { return {}; }

    const Config& config() const { return _config; }

protected:
    Provider(const Config& cfg) : _config(cfg) {}
    Config _config;

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
