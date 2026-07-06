#pragma once

#include <string>
#include <vector>
#include <utility>
#include "agent/tools/tool.hpp"

namespace agent::tools {

struct SearchResult {
    std::string title;
    std::string url;
    std::string snippet;
};

// Parse DuckDuckGo html/ results (result__a links + result__snippet) into
// structured results. Exposed so the parsing can be unit-tested with canned HTML.
std::vector<SearchResult> parse_ddg_html(const std::string& html, size_t max_results);

// Lets the model look things up online. Especially useful for local models
// (Ollama/llama.cpp) with no MCP search server: it gives an otherwise-offline
// model a way to fetch facts beyond its training data and the project files.
class WebSearch : public Tool {
public:
    explicit WebSearch(std::string endpoint) : _endpoint(std::move(endpoint)) {}

    std::string name() const override { return "web_search"; }
    std::string description() const override;
    JSON parameters() const override;
    std::string execute(const JSON& args) override;

private:
    std::string _endpoint;
};

} // namespace agent::tools
