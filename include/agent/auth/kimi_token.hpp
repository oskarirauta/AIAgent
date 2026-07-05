#pragma once

#include <string>
#include <optional>
#include <cstdint>

namespace agent::auth {

struct KimiToken {
    std::string access_token;
    std::string refresh_token;
    std::int64_t expires_at = 0; // unix seconds
    std::string scope;
    std::string token_type = "Bearer";
    std::int64_t expires_in = 0; // original lifetime in seconds
};

std::string credentials_dir(const std::string& home_dir);
std::string token_path(const std::string& home_dir);

std::optional<KimiToken> load_token(const std::string& home_dir);
void save_token(const std::string& home_dir, const KimiToken& token);
void remove_token(const std::string& home_dir);

bool token_needs_refresh(const KimiToken& token, std::int64_t margin_seconds = 300);

} // namespace agent::auth
