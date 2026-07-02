#pragma once

#include <string>
#include <optional>

namespace agent::api {

class Client {
public:
    Client();
    ~Client();

    std::string post(const std::string& url, const std::string& api_key, const std::string& body);
    std::string post(const std::string& url, const std::string& auth_header, const std::string& auth_value, const std::string& body);

private:
    void* curl = nullptr;
};

} // namespace agent::api
