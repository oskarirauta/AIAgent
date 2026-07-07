#include "agent/tools/fetch_url.hpp"

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "common.hpp"
#include "agent/api/client.hpp"
#include "agent/signal_handler.hpp"

namespace agent::tools {

namespace {

constexpr size_t DEFAULT_MAX = 20000;
constexpr size_t HARD_MAX    = 100000;

std::string lower(std::string s) {
    for ( char& c : s ) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The host portion of an http(s) URL: strip scheme, userinfo, port, [] brackets.
std::string url_host(const std::string& url) {
    std::string lo = lower(url);
    size_t start;
    if ( lo.rfind("http://", 0) == 0 ) start = 7;
    else if ( lo.rfind("https://", 0) == 0 ) start = 8;
    else return "";
    size_t end = url.find_first_of("/?#", start);
    std::string auth = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    size_t at = auth.find('@');
    if ( at != std::string::npos ) auth = auth.substr(at + 1);
    if ( !auth.empty() && auth[0] == '[' ) {                 // IPv6 literal [::1]
        size_t rb = auth.find(']');
        return rb == std::string::npos ? auth.substr(1) : auth.substr(1, rb - 1);
    }
    size_t colon = auth.find(':');
    return colon == std::string::npos ? auth : auth.substr(0, colon);
}

bool addr_link_local(const struct sockaddr* sa) {
    if ( sa->sa_family == AF_INET ) {
        uint32_t a = ntohl(reinterpret_cast<const struct sockaddr_in*>(sa)->sin_addr.s_addr);
        return ( a & 0xFFFF0000u ) == 0xA9FE0000u; // 169.254.0.0/16 (incl. cloud metadata)
    }
    if ( sa->sa_family == AF_INET6 ) {
        const uint8_t* b = reinterpret_cast<const struct sockaddr_in6*>(sa)->sin6_addr.s6_addr;
        return b[0] == 0xfe && ( b[1] & 0xc0 ) == 0x80; // fe80::/10
    }
    return false;
}

// True if the host is, or resolves to, a link-local address — catches both a
// literal 169.254.169.254 and an alias like metadata.google.internal.
bool host_link_local(const std::string& host) {
    if ( host.empty()) return false;
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if ( getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res )
        return false;
    bool ll = false;
    for ( struct addrinfo* p = res; p; p = p->ai_next )
        if ( addr_link_local(p->ai_addr)) { ll = true; break; }
    freeaddrinfo(res);
    return ll;
}

// Remove <tag>...</tag> blocks (case-insensitive) — used for script/style whose
// contents are not readable text.
void drop_block(std::string& s, std::string& low, const std::string& open, const std::string& close) {
    size_t pos = 0;
    while (( pos = low.find(open, pos)) != std::string::npos ) {
        size_t end = low.find(close, pos);
        if ( end == std::string::npos ) { s.erase(pos); low.erase(pos); break; }
        end += close.size();
        s.erase(pos, end - pos);
        low.erase(pos, end - pos);
    }
}

bool block_tag(const std::string& name) {
    static const std::vector<std::string> tags = {
        "p", "br", "div", "li", "tr", "ul", "ol", "h1", "h2", "h3", "h4", "h5", "h6",
        "section", "article", "header", "footer", "blockquote", "pre", "table", "hr",
        "nav", "aside", "figure", "figcaption", "dd", "dt", "dl"
    };
    for ( const auto& t : tags )
        if ( name == t )
            return true;
    return false;
}

int hexval(char c) {
    if ( c >= '0' && c <= '9' ) return c - '0';
    if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
    if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
    return -1;
}

// Append a Unicode code point as UTF-8.
void append_utf8(std::string& out, long cp) {
    if ( cp < 0 ) return;
    if ( cp < 0x80 ) out += static_cast<char>(cp);
    else if ( cp < 0x800 ) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if ( cp < 0x10000 ) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string unescape(const std::string& s) {
    std::string out;
    for ( size_t i = 0; i < s.size(); ) {
        if ( s[i] != '&' ) { out += s[i++]; continue; }
        size_t semi = s.find(';', i);
        if ( semi == std::string::npos || semi - i > 12 ) { out += s[i++]; continue; }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if ( !ent.empty() && ent[0] == '#' ) {
            long cp = -1;
            try {
                if ( ent.size() > 1 && ( ent[1] == 'x' || ent[1] == 'X' )) {
                    cp = 0;
                    for ( size_t k = 2; k < ent.size(); ++k ) { int h = hexval(ent[k]); if ( h < 0 ) { cp = -1; break; } cp = cp * 16 + h; }
                } else {
                    cp = std::stol(ent.substr(1));
                }
            } catch ( ... ) { cp = -1; }
            if ( cp >= 0 ) { append_utf8(out, cp); i = semi + 1; continue; }
            out += s[i++]; continue;
        }
        static const std::vector<std::pair<std::string, std::string>> named = {
            { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" },
            { "apos", "'" }, { "nbsp", " " }, { "mdash", "\xE2\x80\x94" },
            { "ndash", "\xE2\x80\x93" }, { "hellip", "\xE2\x80\xA6" },
            { "copy", "\xC2\xA9" }, { "reg", "\xC2\xAE" }, { "trade", "\xE2\x84\xA2" },
            { "quot", "\"" }, { "rsquo", "'" }, { "lsquo", "'" },
            { "ldquo", "\"" }, { "rdquo", "\"" }
        };
        bool hit = false;
        for ( const auto& [k, v] : named )
            if ( ent == k ) { out += v; hit = true; break; }
        if ( hit ) { i = semi + 1; continue; }
        out += s[i++];
    }
    return out;
}

// Collapse trailing spaces per line and runs of 3+ newlines to 2.
std::string tidy(const std::string& s) {
    std::string out;
    int nl_run = 0;
    size_t line_len = 0;
    for ( char c : s ) {
        if ( c == '\n' ) {
            // trim trailing spaces on the line we just finished
            while ( !out.empty() && ( out.back() == ' ' || out.back() == '\t' )) out.pop_back();
            if ( nl_run < 2 ) out += '\n';
            ++nl_run;
            line_len = 0;
        } else if ( c == '\t' ) {
            out += ' ';
            nl_run = 0;
        } else {
            // collapse runs of spaces
            if ( c == ' ' && !out.empty() && out.back() == ' ' && line_len > 0 ) continue;
            out += c;
            nl_run = 0;
            ++line_len;
        }
    }
    // trim leading/trailing whitespace
    size_t a = out.find_first_not_of(" \n\t");
    size_t b = out.find_last_not_of(" \n\t");
    if ( a == std::string::npos ) return "";
    return out.substr(a, b - a + 1);
}

} // namespace

std::string html_to_text(const std::string& html) {
    std::string s = html;
    std::string low = lower(s);
    drop_block(s, low, "<script", "</script>");
    drop_block(s, low, "<style", "</style>");
    drop_block(s, low, "<!--", "-->");

    std::string out;
    for ( size_t i = 0; i < s.size(); ) {
        if ( s[i] != '<' ) { out += s[i++]; continue; }
        size_t end = s.find('>', i);
        if ( end == std::string::npos ) { out += s[i++]; continue; }
        // tag name: skip '<' and optional '/'
        size_t j = i + 1;
        bool closing = ( j < s.size() && s[j] == '/' );
        if ( closing ) ++j;
        std::string name;
        while ( j <= end && ( std::isalnum(static_cast<unsigned char>(s[j])) )) {
            name += static_cast<char>(std::tolower(static_cast<unsigned char>(s[j])));
            ++j;
        }
        if ( block_tag(name))
            out += '\n';
        i = end + 1;
    }
    return tidy(unescape(out));
}

JSON FetchUrl::parameters() const {
    return JSON::Object{
        { "type", "object" },
        { "properties", JSON::Object{
            { "url", JSON::Object{
                { "type", "string" },
                { "description", "the http/https URL to fetch" }
            }},
            { "max_chars", JSON::Object{
                { "type", "integer" },
                { "description", "truncate the text to this many characters (default 20000, max 100000)" }
            }}
        }},
        { "required", JSON::Array{ "url" }}
    };
}

std::string FetchUrl::danger_reason(const JSON& args) const {
    std::string url = common::trim_ws(args.contains("url") ? args["url"].to_string() : "");
    std::string host = url_host(url);
    if ( host_link_local(host))
        return "`" + host + "` resolves to a link-local / cloud-metadata address — "
               "this is a common SSRF target; only allow if you meant to reach it";
    return "";
}

std::string FetchUrl::execute(const JSON& args) {
    std::string url = common::trim_ws(args.contains("url") ? args["url"].to_string() : "");
    if ( url.empty())
        return "error: provide a `url`";
    std::string lo = lower(url);
    if ( lo.rfind("http://", 0) != 0 && lo.rfind("https://", 0) != 0 )
        return "error: only http:// and https:// URLs are allowed";

    size_t max_chars = DEFAULT_MAX;
    if ( args.contains("max_chars") && args["max_chars"] == JSON::TYPE::INT ) {
        long n = static_cast<long>(static_cast<long long>(args["max_chars"]));
        if ( n > 0 ) max_chars = static_cast<size_t>(n);
    }
    if ( max_chars > HARD_MAX ) max_chars = HARD_MAX;

    std::string body;
    try {
        api::Client client;
        std::vector<std::pair<std::string, std::string>> headers = {
            { "User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                            "(KHTML, like Gecko) Chrome/124.0 Safari/537.36" },
            { "Accept", "text/html, text/plain, */*" }
        };
        body = client.get(url, headers, &agent::turn_abort);
    } catch ( const std::exception& e ) {
        return std::string("error: fetch failed: ") + e.what();
    }
    if ( agent::turn_abort.load(std::memory_order_relaxed))
        return "fetch cancelled";
    if ( body.empty())
        return "error: empty response from " + url;

    std::string lb = lower(body.substr(0, 4096));
    bool looks_html = lb.find("<!doctype html") != std::string::npos ||
                      lb.find("<html") != std::string::npos ||
                      lb.find("<body") != std::string::npos ||
                      lb.find("<div") != std::string::npos ||
                      lb.find("</p>") != std::string::npos;

    std::string text = looks_html ? html_to_text(body) : body;
    bool truncated = text.size() > max_chars;
    if ( truncated )
        text = text.substr(0, max_chars);

    std::string header = "fetched " + url + " (" + std::to_string(text.size()) + " chars" +
                         ( truncated ? ", truncated" : "" ) + "):\n\n";
    return header + text;
}

} // namespace agent::tools
