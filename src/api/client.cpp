#include "agent/api/client.hpp"

#include <curl/curl.h>
#include <cstring>
#include <functional>
#include <stdexcept>
#include "throws.hpp"
#include "logger.hpp"
#include "json.hpp"

namespace agent::api {

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
        throws << "http error " << http_code << ": " << format_api_error(response) << std::endl;

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
        throws << "http error " << http_code << ": " << format_api_error(error_body) << std::endl;
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
        throws << "http error " << http_code << ": " << format_api_error(response) << std::endl;

    return response;
}

} // namespace agent::api
