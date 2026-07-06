#include "agent/tools/web_search.hpp"

#include <cctype>
#include <string>
#include <vector>
#include "common.hpp"
#include "agent/api/client.hpp"
#include "agent/signal_handler.hpp"

namespace agent::tools {

namespace {

constexpr size_t MAX_RESULTS_CAP = 10;
constexpr size_t MAX_SNIPPET     = 400;
constexpr size_t MAX_OUTPUT      = 8000;

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for ( unsigned char c : s ) {
        if ( std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' )
            o += static_cast<char>(c);
        else if ( c == ' ' )
            o += '+';
        else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

int hexval(char c) {
    if ( c >= '0' && c <= '9' ) return c - '0';
    if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
    if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string& s) {
    std::string o;
    for ( size_t i = 0; i < s.size(); ++i ) {
        if ( s[i] == '%' && i + 2 < s.size()) {
            int hi = hexval(s[i + 1]), lo = hexval(s[i + 2]);
            if ( hi >= 0 && lo >= 0 ) { o += static_cast<char>(hi * 16 + lo); i += 2; continue; }
        }
        if ( s[i] == '+' ) o += ' ';
        else o += s[i];
    }
    return o;
}

std::string html_unescape(const std::string& s) {
    std::string o;
    for ( size_t i = 0; i < s.size(); ) {
        if ( s[i] == '&' ) {
            if ( s.compare(i, 5, "&amp;") == 0 ) { o += '&'; i += 5; continue; }
            if ( s.compare(i, 4, "&lt;") == 0 ) { o += '<'; i += 4; continue; }
            if ( s.compare(i, 4, "&gt;") == 0 ) { o += '>'; i += 4; continue; }
            if ( s.compare(i, 6, "&quot;") == 0 ) { o += '"'; i += 6; continue; }
            if ( s.compare(i, 6, "&#x27;") == 0 || s.compare(i, 5, "&#39;") == 0 ) {
                o += '\''; i += ( s[i + 2] == 'x' ? 6 : 5 ); continue;
            }
            if ( s.compare(i, 6, "&nbsp;") == 0 ) { o += ' '; i += 6; continue; }
        }
        o += s[i++];
    }
    return o;
}

std::string strip_tags(const std::string& s) {
    std::string o;
    bool in_tag = false;
    for ( char c : s ) {
        if ( c == '<' ) in_tag = true;
        else if ( c == '>' ) in_tag = false;
        else if ( !in_tag ) o += c;
    }
    return o;
}

// Resolve a DuckDuckGo result href (often a //duckduckgo.com/l/?uddg=<url>
// redirect) to the real target URL.
std::string resolve_href(const std::string& raw_href) {
    std::string href = html_unescape(raw_href);
    size_t u = href.find("uddg=");
    if ( u != std::string::npos ) {
        size_t start = u + 5;
        size_t end = href.find('&', start);
        return url_decode(href.substr(start, end == std::string::npos ? std::string::npos : end - start));
    }
    if ( href.rfind("//", 0) == 0 )
        return "https:" + href;
    return href;
}

} // namespace

std::vector<SearchResult> parse_ddg_html(const std::string& html, size_t max_results) {
    std::vector<SearchResult> out;
    size_t pos = 0;
    while ( out.size() < max_results ) {
        size_t a = html.find("result__a", pos);
        if ( a == std::string::npos )
            break;
        size_t tagstart = html.rfind('<', a);
        size_t tagend = html.find('>', a);
        if ( tagend == std::string::npos )
            break;

        std::string url;
        size_t h = html.find("href=\"", tagstart == std::string::npos ? a : tagstart);
        if ( h != std::string::npos && h < tagend ) {
            size_t hs = h + 6;
            size_t he = html.find('"', hs);
            if ( he != std::string::npos )
                url = resolve_href(html.substr(hs, he - hs));
        }

        size_t te = html.find("</a>", tagend);
        std::string title;
        if ( te != std::string::npos )
            title = common::trim_ws(html_unescape(strip_tags(html.substr(tagend + 1, te - tagend - 1))));

        std::string snippet;
        size_t sp = html.find("result__snippet", te == std::string::npos ? tagend : te);
        if ( sp != std::string::npos ) {
            size_t sptag = html.find('>', sp);
            size_t spe = ( sptag == std::string::npos ) ? std::string::npos : html.find("</a>", sptag);
            if ( sptag != std::string::npos && spe != std::string::npos )
                snippet = common::trim_ws(html_unescape(strip_tags(html.substr(sptag + 1, spe - sptag - 1))));
        }
        if ( snippet.size() > MAX_SNIPPET )
            snippet = snippet.substr(0, MAX_SNIPPET) + " …";

        if ( !title.empty() || !url.empty())
            out.push_back({ title, url, snippet });

        pos = ( te == std::string::npos ? tagend : te ) + 1;
    }
    return out;
}

std::string WebSearch::description() const {
    return "Search the web and return the top results (title, URL, snippet). Use it to "
           "look up current information beyond your training data or the project files — "
           "documentation, library versions, API changes, error messages. Give a focused "
           "`query`; read the returned snippets and cite the URLs you rely on.";
}

JSON WebSearch::parameters() const {
    return JSON::Object{
        { "type", "object" },
        { "properties", JSON::Object{
            { "query", JSON::Object{
                { "type", "string" },
                { "description", "the search query" }
            }},
            { "max_results", JSON::Object{
                { "type", "integer" },
                { "description", "how many results to return (1-10, default 5)" }
            }}
        }},
        { "required", JSON::Array{ "query" }}
    };
}

std::string WebSearch::execute(const JSON& args) {
    std::string query = common::trim_ws(args.contains("query") ? args["query"].to_string() : "");
    if ( query.empty())
        return "error: provide a search `query`";

    size_t max_results = 5;
    if ( args.contains("max_results") && args["max_results"] == JSON::TYPE::INT ) {
        long n = static_cast<long>(static_cast<long long>(args["max_results"]));
        if ( n >= 1 )
            max_results = static_cast<size_t>(n);
    }
    if ( max_results > MAX_RESULTS_CAP )
        max_results = MAX_RESULTS_CAP;

    std::string url = _endpoint + ( _endpoint.find('?') == std::string::npos ? "?" : "&" ) +
                      "q=" + url_encode(query);

    std::string html;
    try {
        api::Client client;
        std::vector<std::pair<std::string, std::string>> headers = {
            { "User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                            "(KHTML, like Gecko) Chrome/124.0 Safari/537.36" },
            { "Accept", "text/html" }
        };
        html = client.get(url, headers, &agent::turn_abort);
    } catch ( const std::exception& e ) {
        return std::string("error: web search failed: ") + e.what();
    }
    if ( agent::turn_abort.load(std::memory_order_relaxed))
        return "search cancelled";
    if ( html.empty())
        return "error: empty response from the search endpoint";

    auto results = parse_ddg_html(html, max_results);
    if ( results.empty())
        return "no results for \"" + query + "\"";

    std::string out = std::to_string(results.size()) +
                      ( results.size() == 1 ? " result for \"" : " results for \"" ) + query + "\":\n";
    for ( size_t i = 0; i < results.size(); ++i ) {
        const auto& r = results[i];
        std::string entry = "\n" + std::to_string(i + 1) + ". " + r.title + "\n   " + r.url + "\n";
        if ( !r.snippet.empty())
            entry += "   " + r.snippet + "\n";
        if ( out.size() + entry.size() > MAX_OUTPUT ) break;
        out += entry;
    }
    return out;
}

} // namespace agent::tools
