#include "agent/auth/kimi_token.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <chrono>
#include "json.hpp"
#include "logger.hpp"
#include "common.hpp"

namespace agent::auth {

static std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string credentials_dir(const std::string& home_dir) {
    return home_dir + "/credentials";
}

std::string token_path(const std::string& home_dir) {
    return credentials_dir(home_dir) + "/kimi.json";
}

static void ensure_credentials_dir(const std::string& home_dir) {
    std::string dir = credentials_dir(home_dir);
    if ( !std::filesystem::exists(dir))
        std::filesystem::create_directories(dir);
    chmod(dir.c_str(), 0700);
}

std::optional<KimiToken> load_token(const std::string& home_dir) {
    std::string path = token_path(home_dir);
    if ( !std::filesystem::exists(path))
        return std::nullopt;

    std::ifstream ifd(path, std::ios::in);
    if ( !ifd.is_open()) {
        logger::warning["kimi"] << "failed to open token file: " << path << std::endl;
        return std::nullopt;
    }

    std::stringstream ss;
    ss << ifd.rdbuf();
    std::string raw = ss.str();
    if ( raw.empty())
        return std::nullopt;

    try {
        JSON j = JSON::parse(raw);
        KimiToken token;
        if ( j.contains("access_token"))
            token.access_token = j["access_token"].to_string();
        if ( j.contains("refresh_token"))
            token.refresh_token = j["refresh_token"].to_string();
        if ( j.contains("expires_at") && j["expires_at"] == JSON::TYPE::INT )
            token.expires_at = static_cast<std::int64_t>(j["expires_at"]);
        if ( j.contains("scope"))
            token.scope = j["scope"].to_string();
        if ( j.contains("token_type"))
            token.token_type = j["token_type"].to_string();
        if ( j.contains("expires_in") && j["expires_in"] == JSON::TYPE::INT )
            token.expires_in = static_cast<std::int64_t>(j["expires_in"]);

        if ( token.access_token.empty() || token.refresh_token.empty())
            return std::nullopt;
        return token;
    } catch ( const std::exception& e ) {
        logger::warning["kimi"] << "failed to parse token file: " << e.what() << std::endl;
        return std::nullopt;
    }
}

void save_token(const std::string& home_dir, const KimiToken& token) {
    ensure_credentials_dir(home_dir);
    std::string path = token_path(home_dir);
    std::string tmp = path + ".tmp";

    JSON j = JSON::Object{
        { "access_token", token.access_token },
        { "refresh_token", token.refresh_token },
        { "expires_at", token.expires_at },
        { "scope", token.scope },
        { "token_type", token.token_type },
        { "expires_in", token.expires_in }
    };

    std::string data = j.dump_minified() + "\n";

    {
        std::ofstream ofd(tmp, std::ios::out | std::ios::trunc);
        if ( !ofd.is_open())
            throw std::runtime_error("failed to open temporary token file: " + tmp);
        ofd << data;
        ofd.flush();
    }

    chmod(tmp.c_str(), 0600);
    std::filesystem::rename(tmp, path);
}

void remove_token(const std::string& home_dir) {
    std::string path = token_path(home_dir);
    if ( std::filesystem::exists(path))
        std::filesystem::remove(path);
}

bool token_needs_refresh(const KimiToken& token, std::int64_t margin_seconds) {
    if ( token.expires_at <= 0 )
        return true;
    return now_seconds() + margin_seconds >= token.expires_at;
}

} // namespace agent::auth
