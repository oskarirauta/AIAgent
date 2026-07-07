#include "agent/api/client.hpp"

#include <curl/curl.h>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <netinet/in.h>
#include <sys/socket.h>
#include "throws.hpp"
#include "logger.hpp"
#include "json.hpp"

namespace agent::api {

namespace {

// SSRF guard: reject a connection whose resolved peer is link-local
// (169.254.0.0/16 or fe80::/10) — cloud-metadata and link-local services. Used
// as CURLOPT_OPENSOCKETFUNCTION so it applies on every hop, including redirects.
bool addr_is_link_local(const struct curl_sockaddr* a) {
    if ( a->family == AF_INET ) {
        const struct sockaddr_in* s = reinterpret_cast<const struct sockaddr_in*>(&a->addr);
        uint32_t ip = ntohl(s->sin_addr.s_addr);
        return ( ip & 0xFFFF0000u ) == 0xA9FE0000u;
    }
    if ( a->family == AF_INET6 ) {
        const struct sockaddr_in6* s = reinterpret_cast<const struct sockaddr_in6*>(&a->addr);
        const uint8_t* b = s->sin6_addr.s6_addr;
        return b[0] == 0xfe && ( b[1] & 0xc0 ) == 0x80;
    }
    return false;
}

curl_socket_t guarded_opensocket(void* clientp, curlsocktype purpose, struct curl_sockaddr* address) {
    (void) clientp;
    if ( purpose == CURLSOCKTYPE_IPCXN && addr_is_link_local(address)) {
        logger::warning["http"] << "blocked connection to a link-local address (SSRF guard)" << std::endl;
        return CURL_SOCKET_BAD;
    }
    return ::socket(address->family, address->socktype, address->protocol);
}

// Bounded body sink: append until `cap` bytes, then abort the transfer.
struct CappedSink {
    std::string* out;
    size_t cap;
};
size_t capped_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    CappedSink* s = static_cast<CappedSink*>(userdata);
    size_t n = size * nmemb;
    if ( s->cap && s->out->size() + n > s->cap ) {
        size_t room = s->cap > s->out->size() ? s->cap - s->out->size() : 0;
        s->out->append(ptr, room);
        return 0; // signal "stop" — we have enough; get() treats this as capped
    }
    s->out->append(ptr, n);
    return n;
}

} // namespace

// Extract a human-readable message from a provider error body. OpenAI and
// Anthropic both use {"error":{"message":...}} (Anthropic also {"type":"error"}),
// so pull that out; fall back to the raw body when it is not recognisable JSON.
static std::string format_api_error(const std::string& body) {
    try {
        JSON j = JSON::parse(body);
        if ( j.contains("error")) {
            JSON e = j["error"];
            if ( e == JSON::TYPE::OBJECT && e.contains("message"))
                return e["message"].to_string();
            if ( e == JSON::TYPE::STRING )
                return e.to_string();
        }
        if ( j.contains("message"))
            return j["message"].to_string();
    } catch ( ... ) {
    }
    return body;
}

// A short hint for the common transient HTTP statuses.
static std::string http_hint(long code) {
    if ( code == 429 )
        return " (rate limited — wait a moment and retry, or try a different/paid model; "
               "free models are heavily throttled)";
    if ( code == 503 || code == 529 )
        return " (provider overloaded — retry shortly)";
    return "";
}

static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// A NAMED function, not a lambda: curl_easy_setopt is variadic, and a lambda
// object passed there is not converted to a function pointer (it becomes garbage
// libcurl then calls — a crash). userdata is the std::function<void(chunk)>.
static size_t stream_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* cb = static_cast<std::function<void(const std::string&)>*>(userdata);
    std::string chunk(ptr, size * nmemb);
    (*cb)(chunk);
    return size * nmemb;
}

static int progress_callback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    auto* flag = static_cast<std::atomic<bool>*>(clientp);
    if ( flag && flag->load(std::memory_order_relaxed))
        return 1; // abort the transfer
    return 0;
}

Client::Client() {
    curl = curl_easy_init();
    if ( !curl )
        throws << "failed to initialize libcurl" << std::endl;
}

Client::~Client() {
    if ( curl )
        curl_easy_cleanup(static_cast<CURL*>(curl));
}

std::string Client::post(const std::string& url, const std::string& api_key, const std::string& body, std::atomic<bool>* abort_flag) {
    return post(url, "Authorization", api_key.empty() ? "" : "Bearer " + api_key, body, abort_flag);
}

std::string Client::post(const std::string& url, const std::string& auth_header, const std::string& auth_value, const std::string& body, std::atomic<bool>* abort_flag) {
    return post(url, auth_header, auth_value, {}, body, abort_flag);
}

std::string Client::post(const std::string& url, const std::string& auth_header, const std::string& auth_value,
                         const std::vector<std::pair<std::string, std::string>>& extra_headers,
                         const std::string& body, std::atomic<bool>* abort_flag) {

    CURL* c = static_cast<CURL*>(curl);
    std::string response;
    curl_easy_reset(c);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    for ( const auto& h : extra_headers ) {
        std::string header = h.first + ": " + h.second;
        headers = curl_slist_append(headers, header.c_str());
    }

    if ( !auth_header.empty() && !auth_value.empty()) {
        std::string auth = auth_header + ": " + auth_value;
        headers = curl_slist_append(headers, auth.c_str());
        std::string masked = auth_value.size() > 12
            ? auth_value.substr(0, 6) + "..." + auth_value.substr(auth_value.size() - 4)
            : std::string(auth_value.size(), '*');
        logger::debug["http"] << "auth header: " << auth_header << ": " << masked << std::endl;
    } else {
        logger::debug["http"] << "no auth header added (header=" << auth_header << ", value empty=" << auth_value.empty() << ")" << std::endl;
    }

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);

    if ( abort_flag ) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, abort_flag);
    }

    logger::debug["http"] << "POST " << url << std::endl;
    logger::vverbose["http"] << "POST body\n" << body << std::endl;

    CURLcode res = curl_easy_perform(c);
    curl_slist_free_all(headers);

    if ( res == CURLE_ABORTED_BY_CALLBACK )
        return "";

    if ( res != CURLE_OK )
        throws << "http request failed: " << curl_easy_strerror(res) << std::endl;

    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);

    if ( http_code < 200 || http_code >= 300 )
        throws << "http error " << http_code << ": " << format_api_error(response) << http_hint(http_code) << std::endl;

    return response;
}

void Client::post_stream(const std::string& url, const std::string& auth_header, const std::string& auth_value, const std::string& body, std::function<void(const std::string&)> callback, std::atomic<bool>* abort_flag) {
    post_stream(url, auth_header, auth_value, {}, body, callback, abort_flag);
}

void Client::post_stream(const std::string& url, const std::string& auth_header, const std::string& auth_value,
                         const std::vector<std::pair<std::string, std::string>>& extra_headers,
                         const std::string& body, std::function<void(const std::string&)> callback, std::atomic<bool>* abort_flag) {

    CURL* c = static_cast<CURL*>(curl);
    curl_easy_reset(c);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    for ( const auto& h : extra_headers ) {
        std::string header = h.first + ": " + h.second;
        headers = curl_slist_append(headers, header.c_str());
    }

    if ( !auth_header.empty() && !auth_value.empty()) {
        std::string auth = auth_header + ": " + auth_value;
        headers = curl_slist_append(headers, auth.c_str());
        std::string masked = auth_value.size() > 12
            ? auth_value.substr(0, 6) + "..." + auth_value.substr(auth_value.size() - 4)
            : std::string(auth_value.size(), '*');
        logger::debug["http"] << "auth header: " << auth_header << ": " << masked << std::endl;
    } else {
        logger::debug["http"] << "no auth header added (header=" << auth_header << ", value empty=" << auth_value.empty() << ")" << std::endl;
    }

    // Also accumulate the response (capped) so a non-2xx error body can be shown.
    std::string error_body;
    std::function<void(const std::string&)> sink = [&](const std::string& chunk) {
        if ( error_body.size() < 65536 )
            error_body += chunk;
        callback(chunk);
    };

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, stream_write_callback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    // No hard total timeout for streaming — a long answer (deep thinking + a long
    // reply) legitimately takes minutes. Instead abort only if the connection
    // stalls: less than 1 byte/s for 120s. Plus a bounded connect timeout.
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);

    if ( abort_flag ) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, abort_flag);
    }

    logger::debug["http"] << "POST STREAM " << url << std::endl;
    logger::vverbose["http"] << "POST STREAM body\n" << body << std::endl;

    CURLcode res = curl_easy_perform(c);
    curl_slist_free_all(headers);

    if ( res == CURLE_ABORTED_BY_CALLBACK )
        return;

    if ( res != CURLE_OK )
        throws << "http request failed: " << curl_easy_strerror(res) << std::endl;

    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);

    if ( http_code < 200 || http_code >= 300 )
        throws << "http error " << http_code << ": " << format_api_error(error_body) << http_hint(http_code) << std::endl;
}

std::string Client::get(const std::string& url,
                        const std::vector<std::pair<std::string, std::string>>& extra_headers,
                        std::atomic<bool>* abort_flag,
                        bool ssrf_guard,
                        size_t max_bytes,
                        long timeout_s) {
    CURL* c = static_cast<CURL*>(curl);
    std::string response;
    CappedSink sink { &response, max_bytes };
    curl_easy_reset(c);

    struct curl_slist* headers = nullptr;
    for ( const auto& h : extra_headers ) {
        std::string header = h.first + ": " + h.second;
        headers = curl_slist_append(headers, header.c_str());
    }

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    // Never follow a redirect to a non-HTTP scheme (file:, gopher:, dict:, …).
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    if ( headers )
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    if ( max_bytes ) {
        // Cap via the write callback (stops the transfer at `cap` and keeps the
        // partial body); not CURLOPT_MAXFILESIZE, which errors out with no body.
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, capped_write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    } else {
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    }
    if ( ssrf_guard )
        curl_easy_setopt(c, CURLOPT_OPENSOCKETFUNCTION, guarded_opensocket);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);

    if ( abort_flag ) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, abort_flag);
    }

    logger::vverbose["http"] << "GET " << url << std::endl;

    CURLcode res = curl_easy_perform(c);
    if ( headers )
        curl_slist_free_all(headers);

    // A capped download aborts the write with CURLE_WRITE_ERROR — that is success
    // with the bytes we wanted, not a failure.
    if ( res == CURLE_WRITE_ERROR && max_bytes && !response.empty())
        return response;

    if ( res == CURLE_ABORTED_BY_CALLBACK )
        return "";
    if ( res != CURLE_OK )
        throws << "http request failed: " << curl_easy_strerror(res) << std::endl;

    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    if ( http_code < 200 || http_code >= 300 )
        throws << "http error " << http_code << std::endl;

    return response;
}

std::string Client::post_form(const std::string& url, const std::string& body, std::atomic<bool>* abort_flag) {
    return post_form(url, {}, body, abort_flag);
}

std::string Client::post_form(const std::string& url,
                              const std::vector<std::pair<std::string, std::string>>& extra_headers,
                              const std::string& body, std::atomic<bool>* abort_flag) {

    CURL* c = static_cast<CURL*>(curl);
    std::string response;
    curl_easy_reset(c);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    headers = curl_slist_append(headers, "Accept: application/json");

    for ( const auto& h : extra_headers ) {
        std::string header = h.first + ": " + h.second;
        headers = curl_slist_append(headers, header.c_str());
    }

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);

    if ( abort_flag ) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_callback);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, abort_flag);
    }

    logger::vverbose["http"] << "POST FORM " << url << "\n" << body << std::endl;

    CURLcode res = curl_easy_perform(c);
    curl_slist_free_all(headers);

    if ( res == CURLE_ABORTED_BY_CALLBACK )
        return "";

    if ( res != CURLE_OK )
        throws << "http request failed: " << curl_easy_strerror(res) << std::endl;

    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);

    if ( http_code < 200 || http_code >= 300 )
        throws << "http error " << http_code << ": " << format_api_error(response) << http_hint(http_code) << std::endl;

    return response;
}

} // namespace agent::api
