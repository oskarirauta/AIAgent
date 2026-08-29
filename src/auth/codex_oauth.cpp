#include "agent/auth/codex_oauth.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <ctime>
#include <sys/stat.h>

#include "agent/auth/secure_file.hpp"
#include "json.hpp"
#include "logger.hpp"
#include "throws.hpp"

namespace agent::auth {

namespace {
constexpr const char* TOKEN_URL = "https://auth.openai.com/oauth/token";
constexpr const char* DEVICE_CODE_URL = "https://auth.openai.com/api/accounts/deviceauth/usercode";
constexpr const char* DEVICE_TOKEN_URL = "https://auth.openai.com/api/accounts/deviceauth/token";
constexpr const char* DEVICE_REDIRECT_URI = "https://auth.openai.com/deviceauth/callback";
// Public OAuth client id used by the open-source Codex CLI.
constexpr const char* CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann";

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t parse_timestamp(const std::string& value) {
    std::tm tm{};
    if ( strptime(value.c_str(), "%Y-%m-%dT%H:%M:%S", &tm) == nullptr ) return 0;
    return static_cast<std::int64_t>(timegm(&tm));
}

std::string format_timestamp(std::int64_t value) {
    std::time_t t = static_cast<std::time_t>(value);
    std::tm tm{}; gmtime_r(&t, &tm);
    char out[32]; strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return out;
}

JSON read_auth_json(const std::string& path) {
    std::ifstream f(path);
    if ( !f ) return JSON::Object{};
    std::stringstream ss;
    ss << f.rdbuf();
    return JSON::parse(ss.str());
}

std::string base64url_decode(std::string s) {
    for ( char& c : s ) { if ( c == '-' ) c = '+'; else if ( c == '_' ) c = '/'; }
    while ( s.size() % 4 ) s += '=';
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; int val = 0, bits = -8;
    for ( unsigned char c : s ) {
        if ( c == '=' ) break;
        size_t p = chars.find(c); if ( p == std::string::npos ) continue;
        val = (val << 6) + static_cast<int>(p); bits += 6;
        if ( bits >= 0 ) { out += static_cast<char>((val >> bits) & 0xff); bits -= 8; }
    }
    return out;
}

std::string account_from_id_token(const std::string& token) {
    size_t a = token.find('.'), b = a == std::string::npos ? a : token.find('.', a + 1);
    if ( a == std::string::npos || b == std::string::npos ) return "";
    try {
        JSON claims = JSON::parse(base64url_decode(token.substr(a + 1, b - a - 1)));
        const char* key = "https://api.openai.com/auth.chatgpt_account_id";
        if ( claims.contains(key)) return claims[key].to_string();
    } catch ( ... ) {}
    return "";
}

std::string json_string(const JSON& j, const char* key) {
    return (j.contains(key) && j[key] == JSON::TYPE::STRING) ? j[key].to_string() : "";
}

bool is_pending_code(const std::string& value) {
    return value == "authorization_pending" || value == "deviceauth_authorization_pending";
}

bool is_slow_down_code(const std::string& value) {
    return value == "slow_down" || value == "deviceauth_slow_down";
}
} // namespace

std::string codex_auth_path() {
    const char* codex_home = std::getenv("CODEX_HOME");
    if ( codex_home && *codex_home )
        return std::string(codex_home) + "/auth.json";
    const char* user_home = std::getenv("HOME");
    return std::string(user_home && *user_home ? user_home : "/root") + "/.codex/auth.json";
}

std::optional<CodexToken> load_codex_token() {
    const std::string path = codex_auth_path();
    if ( !std::filesystem::exists(path)) return std::nullopt;
    ensure_owner_only(path, "codex");
    try {
        JSON root = read_auth_json(path);
        if ( !root.contains("tokens") || root["tokens"] != JSON::TYPE::OBJECT )
            return std::nullopt;
        JSON tokens = root["tokens"];
        CodexToken out;
        if ( tokens.contains("access_token")) out.access_token = tokens["access_token"].to_string();
        if ( tokens.contains("refresh_token")) out.refresh_token = tokens["refresh_token"].to_string();
        if ( tokens.contains("id_token")) out.id_token = tokens["id_token"].to_string();
        if ( tokens.contains("account_id")) out.account_id = tokens["account_id"].to_string();
        if ( out.account_id.empty() && !out.id_token.empty()) out.account_id = account_from_id_token(out.id_token);
        if ( root.contains("last_refresh") && root["last_refresh"] == JSON::TYPE::STRING ) {
            out.last_refresh = parse_timestamp(root["last_refresh"].to_string());
        } else if ( root.contains("last_refresh") && root["last_refresh"] == JSON::TYPE::INT ) {
            out.last_refresh = static_cast<std::int64_t>(root["last_refresh"]);
        }
        if ( out.access_token.empty() || out.refresh_token.empty()) return std::nullopt;
        return out;
    } catch ( const std::exception& e ) {
        logger::warning["codex"] << "failed to read Codex credentials: " << e.what() << std::endl;
        return std::nullopt;
    }
}

bool codex_token_needs_refresh(const CodexToken& token, std::int64_t max_age_seconds) {
    return token.last_refresh == 0 || now_seconds() - token.last_refresh >= max_age_seconds;
}

CodexToken refresh_codex_token(api::Client& client, const CodexToken& token) {
    JSON body = JSON::Object{
        { "client_id", CLIENT_ID },
        { "grant_type", "refresh_token" },
        { "refresh_token", token.refresh_token }
    };
    JSON reply = JSON::parse(client.post(TOKEN_URL, "", "", body.dump_minified()));
    if ( !reply.contains("access_token"))
        throws << "Codex token refresh response missing access_token" << std::endl;
    CodexToken out = token;
    out.access_token = reply["access_token"].to_string();
    if ( reply.contains("id_token")) out.id_token = reply["id_token"].to_string();
    if ( !out.id_token.empty()) {
        std::string account = account_from_id_token(out.id_token);
        if ( !account.empty()) out.account_id = account;
    }
    if ( reply.contains("refresh_token") && reply["refresh_token"] == JSON::TYPE::STRING )
        out.refresh_token = reply["refresh_token"].to_string();
    out.last_refresh = now_seconds();
    return out;
}

void save_codex_token(const CodexToken& token) {
    const std::string path = codex_auth_path();
    JSON root = read_auth_json(path); // preserve auth_mode, id_token and future fields
    if ( root != JSON::TYPE::OBJECT ) root = JSON::Object{};
    if ( !root.contains("auth_mode")) root["auth_mode"] = "chatgpt";
    if ( !root.contains("tokens") || root["tokens"] != JSON::TYPE::OBJECT )
        root["tokens"] = JSON::Object{};
    root["tokens"]["access_token"] = token.access_token;
    root["tokens"]["refresh_token"] = token.refresh_token;
    if ( !token.id_token.empty()) root["tokens"]["id_token"] = token.id_token;
    if ( !token.account_id.empty()) root["tokens"]["account_id"] = token.account_id;
    root["last_refresh"] = format_timestamp(token.last_refresh);

    const std::string parent = std::filesystem::path(path).parent_path().string();
    std::filesystem::create_directories(parent);
    chmod(parent.c_str(), 0700);
    const std::string tmp = path + ".ai-agent.tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if ( !f ) throws << "could not write Codex credentials" << std::endl;
        f << root.dump(2) << '\n';
    }
    chmod(tmp.c_str(), 0600);
    std::filesystem::rename(tmp, path);
    chmod(path.c_str(), 0600);
}

CodexToken login_codex_device(api::Client& client) {
    JSON start_body = JSON::Object{
        { "client_id", CLIENT_ID }
    };
    JSON start = JSON::parse(client.post(DEVICE_CODE_URL, "", "", start_body.dump_minified()));
    if ( !start.contains("device_auth_id") || !start.contains("user_code"))
        throws << "Codex device login response missing required fields" << std::endl;
    const std::string device_id = start["device_auth_id"].to_string();
    const std::string user_code = start["user_code"].to_string();
    std::string verify = start.contains("verification_uri")
        ? start["verification_uri"].to_string() : "https://auth.openai.com/codex/device";
    std::int64_t interval = start.contains("interval") ? static_cast<std::int64_t>(start["interval"]) : 5;
    std::int64_t expires = start.contains("expires_in") ? static_cast<std::int64_t>(start["expires_in"]) : 900;

    std::cout << "Codex login required.\n\nOpen this URL in a browser:\n  " << verify
              << "\n\nEnter this code:\n  " << user_code
              << "\n\nWaiting for authorization..." << std::endl;
    const std::int64_t deadline = now_seconds() + expires;
    JSON grant;
    while ( now_seconds() < deadline ) {
        std::this_thread::sleep_for(std::chrono::seconds(interval > 0 ? interval : 5));
        JSON poll_body = JSON::Object{
            { "device_auth_id", device_id }, { "user_code", user_code }
        };
        std::string raw = client.post_json_raw(DEVICE_TOKEN_URL, poll_body.dump_minified());
        try { if ( !raw.empty()) grant = JSON::parse(raw); } catch ( ... ) { continue; }
        if ( grant.contains("authorization_code") && grant.contains("code_verifier")) break;
        const std::string error = json_string(grant, "error");
        const std::string code = json_string(grant, "code");
        if ( is_pending_code(error) || is_pending_code(code))
            continue;
        if ( is_slow_down_code(error) || is_slow_down_code(code)) {
            interval += 5;
            continue;
        }
        if ( !error.empty() || !code.empty() ) {
            const std::string detail = !grant.empty() ? grant.dump(2) : (!raw.empty() ? raw : code);
            throws << "Codex login failed: " << detail << std::endl;
        }
    }
    if ( !grant.contains("authorization_code")) throws << "Codex login timed out" << std::endl;

    JSON exchange_body = JSON::Object{
        { "grant_type", "authorization_code" }, { "client_id", CLIENT_ID },
        { "code", grant["authorization_code"].to_string() },
        { "code_verifier", grant["code_verifier"].to_string() },
        { "redirect_uri", DEVICE_REDIRECT_URI }
    };
    JSON exchange = JSON::parse(client.post(TOKEN_URL, "", "", exchange_body.dump_minified()));
    if ( !exchange.contains("access_token") || !exchange.contains("refresh_token"))
        throws << "Codex login token response missing required fields" << std::endl;
    CodexToken token;
    token.access_token = exchange["access_token"].to_string();
    token.refresh_token = exchange["refresh_token"].to_string();
    if ( exchange.contains("id_token")) token.id_token = exchange["id_token"].to_string();
    token.account_id = account_from_id_token(token.id_token);
    token.last_refresh = now_seconds();
    save_codex_token(token);
    return token;
}

} // namespace agent::auth
