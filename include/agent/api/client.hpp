#pragma once

#include <string>
#include <optional>
#include <functional>
#include <atomic>
#include <vector>
#include <utility>

namespace agent::api {

class Client {
public:
    Client();
    ~Client();

    std::string post(const std::string& url, const std::string& api_key, const std::string& body, std::atomic<bool>* abort_flag = nullptr);
    std::string post(const std::string& url, const std::string& auth_header, const std::string& auth_value, const std::string& body, std::atomic<bool>* abort_flag = nullptr);
    std::string post(const std::string& url, const std::string& auth_header, const std::string& auth_value,
                     const std::vector<std::pair<std::string, std::string>>& extra_headers,
                     const std::string& body, std::atomic<bool>* abort_flag = nullptr);
    // Plain HTTP GET (follows redirects). Used by tools like web_search; not for
    // the provider APIs. Returns the response body; throws on a non-2xx status.
    std::string get(const std::string& url,
                    const std::vector<std::pair<std::string, std::string>>& extra_headers = {},
                    std::atomic<bool>* abort_flag = nullptr);

    std::string post_form(const std::string& url, const std::string& body, std::atomic<bool>* abort_flag = nullptr);
    std::string post_form(const std::string& url,
                          const std::vector<std::pair<std::string, std::string>>& extra_headers,
                          const std::string& body, std::atomic<bool>* abort_flag = nullptr);
    void post_stream(const std::string& url, const std::string& auth_header, const std::string& auth_value, const std::string& body, std::function<void(const std::string&)> callback, std::atomic<bool>* abort_flag = nullptr);
    void post_stream(const std::string& url, const std::string& auth_header, const std::string& auth_value,
                     const std::vector<std::pair<std::string, std::string>>& extra_headers,
                     const std::string& body, std::function<void(const std::string&)> callback, std::atomic<bool>* abort_flag = nullptr);

private:
    void* curl = nullptr;
};

} // namespace agent::api
