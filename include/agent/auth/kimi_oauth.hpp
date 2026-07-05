#pragma once

#include <string>
#include <optional>
#include "agent/api/client.hpp"
#include "agent/auth/kimi_token.hpp"

namespace agent::auth {

struct DeviceAuthorization {
    std::string user_code;
    std::string device_code;
    std::string verification_uri;
    std::string verification_uri_complete;
    std::int64_t expires_in = 0;
    std::int64_t interval = 5;
};

enum class PollStatus {
    success,
    pending,
    expired,
    denied,
    error
};

struct PollResult {
    PollStatus status = PollStatus::error;
    KimiToken token;
    std::string error_description;
};

DeviceAuthorization request_device_authorization(
    const std::string& oauth_host,
    const std::string& client_id,
    const std::string& home_dir,
    api::Client& client);

PollResult poll_device_token(
    const std::string& oauth_host,
    const std::string& client_id,
    const std::string& device_code,
    const std::string& home_dir,
    api::Client& client);

KimiToken refresh_access_token(
    const std::string& oauth_host,
    const std::string& client_id,
    const std::string& refresh_token,
    const std::string& home_dir,
    api::Client& client);

std::string normalize_oauth_host(std::string host);

// Kimi Code device identity headers (X-Msh-* + User-Agent). These make the
// OAuth server recognise the client as the official Kimi Code CLI and route the
// user to the correct verification page. The device id is persisted under
// home_dir so it stays stable across runs.
std::vector<std::pair<std::string, std::string>> create_device_headers(const std::string& home_dir);

} // namespace agent::auth
