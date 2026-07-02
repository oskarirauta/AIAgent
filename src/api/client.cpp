#include "client.hpp"

#include <curl/curl.h>
#include <cstring>
#include <stdexcept>
#include "throws.hpp"
#include "logger.hpp"

namespace agent::api {

static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
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

std::string Client::post(const std::string& url, const std::string& api_key, const std::string& body) {
    return post(url, "Authorization", api_key.empty() ? "" : "Bearer " + api_key, body);
}

std::string Client::post(const std::string& url, const std::string& auth_header, const std::string& auth_value, const std::string& body) {

    CURL* c = static_cast<CURL*>(curl);
    std::string response;
    curl_easy_reset(c);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    if ( !auth_header.empty() && !auth_value.empty()) {
        std::string auth = auth_header + ": " + auth_value;
        headers = curl_slist_append(headers, auth.c_str());
    }

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);

    logger::vverbose["http"] << "POST " << url << "\n" << body << std::endl;

    CURLcode res = curl_easy_perform(c);
    curl_slist_free_all(headers);

    if ( res != CURLE_OK )
        throws << "http request failed: " << curl_easy_strerror(res) << std::endl;

    long http_code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);

    if ( http_code < 200 || http_code >= 300 )
        throws << "http error " << http_code << ": " << response << std::endl;

    return response;
}

} // namespace agent::api
