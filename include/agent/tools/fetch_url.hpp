#pragma once

#include <string>
#include "agent/tools/tool.hpp"

namespace agent::tools {

// Convert an HTML document to readable plain text: drop <script>/<style>, turn
// block-level tags into line breaks, strip remaining tags, decode entities and
// collapse runs of blank lines. Exposed for testing.
std::string html_to_text(const std::string& html);

// Fetch a URL and return its text content (HTML is reduced to readable text).
// Complements web_search: the model finds a link, then reads it. Especially
// useful for local models with no MCP fetch server.
class FetchUrl : public Tool {
public:
    std::string name() const override { return "fetch_url"; }
    std::string description() const override {
        return "Fetch a web page or text document by URL and return its text content "
               "(HTML is stripped to readable text). Use it to read documentation or a "
               "page found via web_search. Only http:// and https:// URLs are allowed.";
    }
    JSON parameters() const override;
    // A URL resolving to a link-local / cloud-metadata address (169.254.0.0/16,
    // fe80::/10) is flagged dangerous so it needs confirmation — an SSRF guard.
    // localhost / private ranges are allowed (dev servers). Insecure mode skips it.
    std::string danger_reason(const JSON& args) const override;
    std::string execute(const JSON& args) override;
};

} // namespace agent::tools
