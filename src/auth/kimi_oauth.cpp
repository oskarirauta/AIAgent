#include "agent/auth/kimi_oauth.hpp"

#include <curl/curl.h>
#include <thread>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include "json.hpp"
#include "logger.hpp"
#include "throws.hpp"

namespace agent::auth {

// Identity of the official Kimi Code CLI. The OAuth server keys the verification
// page off X-Msh-Platform, so this must match the real client.
static constexpr const char* KIMI_CLI_PLATFORM = "kimi_code_cli";
static constexpr const char* KIMI_CLI_PRODUCT = "kimi-code-cli";
static constexpr const char* KIMI_CLI_VERSION = "0.22.3";

// Strip non-printable/non-ASCII characters so values are always valid header
// content; fall back to a placeholder when nothing usable remains.
static std::string ascii_header(const std::string& value, const std::string& fallback = "unknown") {
    std::string cleaned;
    for ( unsigned char c : value ) {
        if ( c >= 0x20 && c <= 0x7E )
            cleaned += static_cast<char>(c);
    }
    // trim surrounding whitespace
    size_t start = cleaned.find_first_not_of(" \t");
    size_t end = cleaned.find_last_not_of(" \t");
    if ( start == std::string::npos )
        return fallback;
    cleaned = cleaned.substr(start, end - start + 1);
    return cleaned.empty() ? fallback : cleaned;
}

static std::string generate_uuid_v4() {
    static std::mt19937_64 gen{std::random_device{}()};
    std::uniform_int_distribution<int> hex(0, 15);
    std::uniform_int_distribution<int> variant(8, 11);
    std::ostringstream ss;
    ss << std::hex;
    for ( int i = 0; i < 8; ++i ) ss << hex(gen);
    ss << "-";
    for ( int i = 0; i < 4; ++i ) ss << hex(gen);
    ss << "-4";
    for ( int i = 0; i < 3; ++i ) ss << hex(gen);
    ss << "-" << variant(gen);
    for ( int i = 0; i < 3; ++i ) ss << hex(gen);
    ss << "-";
    for ( int i = 0; i < 12; ++i ) ss << hex(gen);
    return ss.str();
}

// Read the stable device id from home_dir/device_id, minting and persisting a
// new one the first time.
static std::string device_id(const std::string& home_dir) {
    std::string path = home_dir + "/device_id";
    if ( std::filesystem::exists(path)) {
        std::ifstream ifd(path);
        std::string id;
        std::getline(ifd, id);
        id = ascii_header(id, "");
        if ( !id.empty())
            return id;
    }

    std::string id = generate_uuid_v4();
    try {
        if ( !std::filesystem::exists(home_dir))
            std::filesystem::create_directories(home_dir);
        std::ofstream ofd(path, std::ios::out | std::ios::trunc);
        if ( ofd.is_open()) {
            ofd << id;
            ofd.flush();
        }
        chmod(path.c_str(), 0600);
    } catch ( const std::exception& e ) {
        logger::warning["kimi"] << "failed to persist device id: " << e.what() << std::endl;
    }
    return id;
}

std::vector<std::pair<std::string, std::string>> create_device_headers(const std::string& home_dir) {
    std::string host_name = "unknown";
    std::string os_sysname = "Linux";
    std::string os_release = "unknown";
    std::string os_arch = "unknown";

    char hn[256];
    if ( gethostname(hn, sizeof(hn)) == 0 )
        host_name = ascii_header(hn);

    struct utsname uts;
    if ( uname(&uts) == 0 ) {
        os_sysname = ascii_header(uts.sysname, "Linux");
        os_release = ascii_header(uts.release);
        os_arch = ascii_header(uts.machine);
    }

    std::string device_model = ascii_header(os_sysname + " " + os_release + " " + os_arch);

    return {
        { "User-Agent", std::string(KIMI_CLI_PRODUCT) + "/" + KIMI_CLI_VERSION },
        { "X-Msh-Platform", KIMI_CLI_PLATFORM },
        { "X-Msh-Version", KIMI_CLI_VERSION },
        { "X-Msh-Device-Name", host_name },
        { "X-Msh-Device-Model", device_model },
        { "X-Msh-Os-Version", os_release },
        { "X-Msh-Device-Id", device_id(home_dir) }
    };
}

static std::string url_encode(const std::string& value) {
    CURL* curl = curl_easy_init();
    if ( !curl )
        return value;
    char* escaped = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    std::string result(escaped ? escaped : "");
    curl_free(escaped);
    curl_easy_cleanup(curl);
    return result;
}

static std::string form_encode(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string body;
    for ( size_t i = 0; i < fields.size(); ++i ) {
        if ( i > 0 ) body += "&";
        body += url_encode(fields[i].first);
        body += "=";
        body += url_encode(fields[i].second);
    }
    return body;
}

static KimiToken token_from_json(const JSON& j) {
    KimiToken token;
    if ( j.contains("access_token"))
        token.access_token = j["access_token"].to_string();
    if ( j.contains("refresh_token"))
        token.refresh_token = j["refresh_token"].to_string();
    if ( j.contains("scope"))
        token.scope = j["scope"].to_string();
    if ( j.contains("token_type"))
        token.token_type = j["token_type"].to_string();
    if ( j.contains("expires_in") && j["expires_in"] == JSON::TYPE::INT )
        token.expires_in = static_cast<std::int64_t>(j["expires_in"]);
    else if ( j.contains("expires_in") && j["expires_in"] == JSON::TYPE::FLOAT )
        token.expires_in = static_cast<std::int64_t>(static_cast<long double>(j["expires_in"]));

    if ( token.token_type.empty())
        token.token_type = "Bearer";

    token.expires_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + token.expires_in;
    return token;
}

std::string normalize_oauth_host(std::string host) {
    while ( !host.empty() && host.back() == '/' )
        host.pop_back();
    return host;
}

DeviceAuthorization request_device_authorization(
    const std::string& oauth_host,
    const std::string& client_id,
    const std::string& home_dir,
    api::Client& client) {

    std::string url = normalize_oauth_host(oauth_host) + "/api/oauth/device_authorization";
    std::string body = form_encode({{"client_id", client_id}});

    std::string response = client.post_form(url, create_device_headers(home_dir), body);
    JSON j = JSON::parse(response);

    DeviceAuthorization auth;
    if ( j.contains("user_code"))
        auth.user_code = j["user_code"].to_string();
    if ( j.contains("device_code"))
        auth.device_code = j["device_code"].to_string();
    if ( j.contains("verification_uri"))
        auth.verification_uri = j["verification_uri"].to_string();
    if ( j.contains("verification_uri_complete"))
        auth.verification_uri_complete = j["verification_uri_complete"].to_string();
    if ( j.contains("expires_in") && j["expires_in"] == JSON::TYPE::INT )
        auth.expires_in = static_cast<std::int64_t>(j["expires_in"]);
    if ( j.contains("interval") && j["interval"] == JSON::TYPE::INT )
        auth.interval = static_cast<std::int64_t>(j["interval"]);
    else if ( j.contains("interval") && j["interval"] == JSON::TYPE::FLOAT )
        auth.interval = static_cast<std::int64_t>(static_cast<long double>(j["interval"]));

    if ( auth.user_code.empty() || auth.device_code.empty() || auth.verification_uri_complete.empty())
        throws << "device authorization response missing required fields" << std::endl;

    return auth;
}

PollResult poll_device_token(
    const std::string& oauth_host,
    const std::string& client_id,
    const std::string& device_code,
    const std::string& home_dir,
    api::Client& client) {

    std::string url = normalize_oauth_host(oauth_host) + "/api/oauth/token";
    std::string body = form_encode({
        {"client_id", client_id},
        {"device_code", device_code},
        {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}
    });

    // RFC 8628 returns the polling states (authorization_pending, slow_down,
    // access_denied, expired_token) as HTTP 400 + a JSON body, so use the raw
    // variant that keeps the body instead of throwing the error away.
    std::string response = client.post_form_raw(url, create_device_headers(home_dir), body);

    PollResult result;
    JSON j;
    try {
        if ( !response.empty())
            j = JSON::parse(response);
    } catch ( ... ) {
        // leave j empty
    }

    // If the response is valid JSON with an access_token, success.
    if ( j.contains("access_token")) {
        result.status = PollStatus::success;
        result.token = token_from_json(j);
        return result;
    }

    if ( !j.empty() && j.contains("error")) {
        std::string error = j["error"].to_string();
        if ( j.contains("error_description"))
            result.error_description = j["error_description"].to_string();
        if ( error == "authorization_pending" || error == "slow_down" ) {
            result.status = PollStatus::pending;
            result.slow_down = ( error == "slow_down" );
            return result;
        }
        if ( error == "expired_token" ) {
            result.status = PollStatus::expired;
            return result;
        }
        if ( error == "access_denied" ) {
            result.status = PollStatus::denied;
            if ( result.error_description.empty())
                result.error_description = "access denied";
            return result;
        }
    }

    // For non-2xx responses the client throws; treat unknown errors as pending
    // so polling continues unless the caller decides otherwise.
    result.status = PollStatus::pending;
    if ( result.error_description.empty())
        result.error_description = "authorization pending";
    return result;
}

KimiToken refresh_access_token(
    const std::string& oauth_host,
    const std::string& client_id,
    const std::string& refresh_token,
    const std::string& home_dir,
    api::Client& client) {

    std::string url = normalize_oauth_host(oauth_host) + "/api/oauth/token";
    std::string body = form_encode({
        {"client_id", client_id},
        {"refresh_token", refresh_token},
        {"grant_type", "refresh_token"}
    });

    std::string response = client.post_form(url, create_device_headers(home_dir), body);
    JSON j = JSON::parse(response);

    if ( !j.contains("access_token"))
        throws << "refresh token response missing access_token" << std::endl;

    return token_from_json(j);
}

} // namespace agent::auth
