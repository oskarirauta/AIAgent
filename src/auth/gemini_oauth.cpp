#include "agent/auth/gemini_oauth.hpp"
#include "agent/auth/secure_file.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <curl/curl.h>

#include "agent/config.hpp"
#include "common.hpp"
#include "json.hpp"
#include "logger.hpp"
#include "throws.hpp"

namespace agent::auth {

namespace {

// Official OAuth client ID and secret used by Google Gemini CLI
constexpr const char* CLIENT_ID = "681255809395-oo8ft2oprdrnp9e3aqf6av3hmdib135j.apps.googleusercontent.com";
constexpr const char* CLIENT_SECRET = "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";
constexpr const char* TOKEN_URL = "https://oauth2.googleapis.com/token";
constexpr const char* AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth";
constexpr const char* REDIRECT_URI = "https://codeassist.google.com/authcode";
constexpr const char* OAUTH_SCOPES =
    "https://www.googleapis.com/auth/cloud-platform "
    "https://www.googleapis.com/auth/userinfo.email "
    "https://www.googleapis.com/auth/userinfo.profile";

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string url_encode(const std::string& value) {
    CURL* c = curl_easy_init();
    if ( !c ) return value;
    char* escaped = curl_easy_escape(c, value.c_str(), static_cast<int>(value.size()));
    std::string res(escaped ? escaped : "");
    curl_free(escaped);
    curl_easy_cleanup(c);
    return res;
}

std::string random_string(size_t length) {
    static const char charset[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-._~";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dist(0, sizeof(charset) - 2);
    std::string s;
    s.reserve(length);
    for ( size_t i = 0; i < length; ++i )
        s += charset[dist(gen)];
    return s;
}

std::array<std::uint8_t, 32> sha256(const std::string& input) {
    std::array<std::uint32_t, 64> k = {{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    }};

    std::uint32_t h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    std::uint32_t h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;

    std::vector<std::uint8_t> data(input.begin(), input.end());
    std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8;
    data.push_back(0x80);
    while ((data.size() % 64) != 56)
        data.push_back(0);
    for ( int i = 7; i >= 0; --i )
        data.push_back(static_cast<std::uint8_t>(bit_len >> (i * 8)));

    auto rotr = [](std::uint32_t x, std::uint32_t n) { return (x >> n) | (x << (32 - n)); };

    for ( size_t i = 0; i < data.size(); i += 64 ) {
        std::array<std::uint32_t, 64> w{};
        for ( size_t j = 0; j < 16; ++j ) {
            w[j] = (static_cast<std::uint32_t>(data[i + j * 4]) << 24) |
                   (static_cast<std::uint32_t>(data[i + j * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(data[i + j * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(data[i + j * 4 + 3]);
        }
        for ( size_t j = 16; j < 64; ++j ) {
            std::uint32_t s0 = rotr(w[j - 15], 7) ^ rotr(w[j - 15], 18) ^ (w[j - 15] >> 3);
            std::uint32_t s1 = rotr(w[j - 2], 17) ^ rotr(w[j - 2], 19) ^ (w[j - 2] >> 10);
            w[j] = w[j - 16] + s0 + w[j - 7] + s1;
        }

        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
        for ( size_t j = 0; j < 64; ++j ) {
            std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t temp1 = h + s1 + ch + k[j] + w[j];
            std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t temp2 = s0 + maj;

            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        h0 += a; h1 += b; h2 += c; h3 += d;
        h4 += e; h5 += f; h6 += g; h7 += h;
    }

    std::array<std::uint8_t, 32> out{};
    for ( size_t i = 0; i < 8; ++i ) {
        std::uint32_t val = (i == 0 ? h0 : i == 1 ? h1 : i == 2 ? h2 : i == 3 ? h3 :
                             i == 4 ? h4 : i == 5 ? h5 : i == 6 ? h6 : h7);
        out[i * 4] = static_cast<std::uint8_t>(val >> 24);
        out[i * 4 + 1] = static_cast<std::uint8_t>(val >> 16);
        out[i * 4 + 2] = static_cast<std::uint8_t>(val >> 8);
        out[i * 4 + 3] = static_cast<std::uint8_t>(val);
    }
    return out;
}

std::string base64_url_encode(const std::string& input) {
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for ( unsigned char c : input ) {
        val = (val << 8) + c;
        valb += 8;
        while ( valb >= 0 ) {
            out.push_back(chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if ( valb > -6 )
        out.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
    for ( char& c : out ) {
        if ( c == '+' ) c = '-';
        else if ( c == '/' ) c = '_';
    }
    return out;
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

std::string email_from_id_token(const std::string& token) {
    size_t a = token.find('.'), b = a == std::string::npos ? a : token.find('.', a + 1);
    if ( a == std::string::npos || b == std::string::npos ) return "";
    try {
        JSON claims = JSON::parse(base64url_decode(token.substr(a + 1, b - a - 1)));
        if ( claims.contains("email") && claims["email"] == JSON::TYPE::STRING )
            return claims["email"].to_string();
    } catch ( ... ) {}
    return "";
}

JSON read_json_file(const std::string& path) {
    std::ifstream f(path);
    if ( !f ) return JSON::Object{};
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        return JSON::parse(ss.str());
    } catch ( ... ) {
        return JSON::Object{};
    }
}

GeminiToken token_from_json(const JSON& j) {
    GeminiToken token;
    if ( j.contains("api_key") && j["api_key"] == JSON::TYPE::STRING )
        token.api_key = j["api_key"].to_string();
    if ( j.contains("access_token") && j["access_token"] == JSON::TYPE::STRING )
        token.access_token = j["access_token"].to_string();
    if ( j.contains("refresh_token") && j["refresh_token"] == JSON::TYPE::STRING )
        token.refresh_token = j["refresh_token"].to_string();
    if ( j.contains("id_token") && j["id_token"] == JSON::TYPE::STRING )
        token.id_token = j["id_token"].to_string();
    if ( j.contains("token_type") && j["token_type"] == JSON::TYPE::STRING )
        token.token_type = j["token_type"].to_string();
    if ( j.contains("scope") && j["scope"] == JSON::TYPE::STRING )
        token.scope = j["scope"].to_string();

    if ( j.contains("expiry_date") ) {
        if ( j["expiry_date"] == JSON::TYPE::INT )
            token.expiry_date_ms = static_cast<std::int64_t>(j["expiry_date"]);
        else if ( j["expiry_date"] == JSON::TYPE::FLOAT )
            token.expiry_date_ms = static_cast<std::int64_t>(static_cast<long double>(j["expiry_date"]));
    } else if ( j.contains("expires_at") ) {
        // Seconds to milliseconds
        if ( j["expires_at"] == JSON::TYPE::INT )
            token.expiry_date_ms = static_cast<std::int64_t>(j["expires_at"]) * 1000;
        else if ( j["expires_at"] == JSON::TYPE::FLOAT )
            token.expiry_date_ms = static_cast<std::int64_t>(static_cast<long double>(j["expires_at"])) * 1000;
    }

    if ( !token.id_token.empty())
        token.email = email_from_id_token(token.id_token);

    return token;
}

} // namespace

std::string gemini_cli_auth_path() {
    const char* user_home = std::getenv("HOME");
    return std::string(user_home && *user_home ? user_home : "/root") + "/.gemini/oauth_creds.json";
}

std::string gemini_agent_auth_path(const std::string& home_dir) {
    std::string base = home_dir.empty() ? Config::default_home_dir() : home_dir;
    return base + "/credentials/gemini.json";
}

std::optional<GeminiToken> load_gemini_token(const std::string& home_dir) {
    // 1. Try agent's own credentials path
    std::string agent_path = gemini_agent_auth_path(home_dir);
    if ( std::filesystem::exists(agent_path) ) {
        ensure_owner_only(agent_path, "gemini");
        JSON j = read_json_file(agent_path);
        GeminiToken tok = token_from_json(j);
        if ( !tok.access_token.empty() || !tok.refresh_token.empty() || !tok.api_key.empty() )
            return tok;
    }

    // 2. Try Gemini CLI path ~/.gemini/oauth_creds.json
    std::string cli_path = gemini_cli_auth_path();
    if ( std::filesystem::exists(cli_path) ) {
        ensure_owner_only(cli_path, "gemini");
        JSON j = read_json_file(cli_path);
        GeminiToken tok = token_from_json(j);
        if ( !tok.access_token.empty() || !tok.refresh_token.empty() || !tok.api_key.empty() )
            return tok;
    }

    return std::nullopt;
}

bool gemini_token_needs_refresh(const GeminiToken& token, std::int64_t margin_seconds) {
    if ( token.refresh_token.empty() )
        return false;
    if ( token.expiry_date_ms == 0 )
        return true;
    return now_ms() + (margin_seconds * 1000) >= token.expiry_date_ms;
}

GeminiToken refresh_gemini_token(api::Client& client, const GeminiToken& token) {
    if ( token.refresh_token.empty() )
        throws << "cannot refresh Gemini token: refresh_token is empty" << std::endl;

    JSON body = JSON::Object{
        { "client_id", CLIENT_ID },
        { "client_secret", CLIENT_SECRET },
        { "grant_type", "refresh_token" },
        { "refresh_token", token.refresh_token }
    };

    std::string resp_str = client.post(TOKEN_URL, "", "", body.dump_minified());
    if ( resp_str.empty() )
        throws << "empty response refreshing Gemini token" << std::endl;

    JSON reply = JSON::parse(resp_str);
    if ( !reply.contains("access_token") ) {
        std::string err = reply.contains("error") ? reply["error"].to_string() : resp_str;
        throws << "Gemini token refresh failed: " << err << std::endl;
    }

    GeminiToken updated = token;
    updated.access_token = reply["access_token"].to_string();

    std::int64_t expires_in = 3600;
    if ( reply.contains("expires_in") ) {
        if ( reply["expires_in"] == JSON::TYPE::INT )
            expires_in = static_cast<std::int64_t>(reply["expires_in"]);
        else if ( reply["expires_in"] == JSON::TYPE::FLOAT )
            expires_in = static_cast<std::int64_t>(static_cast<long double>(reply["expires_in"]));
    }
    updated.expiry_date_ms = now_ms() + (expires_in * 1000);

    if ( reply.contains("refresh_token") && reply["refresh_token"] == JSON::TYPE::STRING )
        updated.refresh_token = reply["refresh_token"].to_string();
    if ( reply.contains("id_token") && reply["id_token"] == JSON::TYPE::STRING ) {
        updated.id_token = reply["id_token"].to_string();
        updated.email = email_from_id_token(updated.id_token);
    }
    if ( reply.contains("token_type") && reply["token_type"] == JSON::TYPE::STRING )
        updated.token_type = reply["token_type"].to_string();

    save_gemini_token("", updated);
    return updated;
}

void save_gemini_token(const std::string& home_dir, const GeminiToken& token) {
    // If ~/.gemini/oauth_creds.json exists, update it to keep Gemini CLI in sync
    std::string cli_path = gemini_cli_auth_path();
    if ( std::filesystem::exists(cli_path) ) {
        JSON root = read_json_file(cli_path);
        if ( root != JSON::TYPE::OBJECT ) root = JSON::Object{};
        root["access_token"] = token.access_token;
        root["refresh_token"] = token.refresh_token;
        root["token_type"] = token.token_type.empty() ? "Bearer" : token.token_type;
        root["expiry_date"] = static_cast<long long>(token.expiry_date_ms);
        if ( !token.id_token.empty() ) root["id_token"] = token.id_token;
        if ( !token.scope.empty() ) root["scope"] = token.scope;

        const std::string parent = std::filesystem::path(cli_path).parent_path().string();
        std::filesystem::create_directories(parent);
        chmod(parent.c_str(), 0700);
        const std::string tmp = cli_path + ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            if ( f ) f << root.dump(2) << '\n';
        }
        chmod(tmp.c_str(), 0600);
        std::filesystem::rename(tmp, cli_path);
        chmod(cli_path.c_str(), 0600);
    }

    // Also write to agent's own credentials path
    std::string agent_path = gemini_agent_auth_path(home_dir);
    const std::string parent = std::filesystem::path(agent_path).parent_path().string();
    std::filesystem::create_directories(parent);
    chmod(parent.c_str(), 0700);

    JSON root = read_json_file(agent_path);
    if ( root != JSON::TYPE::OBJECT ) root = JSON::Object{};
    if ( !token.access_token.empty() ) root["access_token"] = token.access_token;
    if ( !token.refresh_token.empty() ) root["refresh_token"] = token.refresh_token;
    if ( !token.token_type.empty() ) root["token_type"] = token.token_type;
    if ( token.expiry_date_ms > 0 ) root["expiry_date"] = static_cast<long long>(token.expiry_date_ms);
    if ( !token.scope.empty() ) root["scope"] = token.scope;
    if ( !token.id_token.empty() ) root["id_token"] = token.id_token;
    if ( !token.email.empty() ) root["email"] = token.email;
    if ( !token.api_key.empty() ) root["api_key"] = token.api_key;

    const std::string tmp = agent_path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if ( !f ) throws << "could not write Gemini credentials to " << agent_path << std::endl;
        f << root.dump(2) << '\n';
    }
    chmod(tmp.c_str(), 0600);
    std::filesystem::rename(tmp, agent_path);
    chmod(agent_path.c_str(), 0600);
}

void save_gemini_api_key(const std::string& home_dir, const std::string& api_key) {
    auto tok = load_gemini_token(home_dir);
    GeminiToken token = tok.value_or(GeminiToken{});
    token.api_key = api_key;
    save_gemini_token(home_dir, token);
}

std::string load_gemini_api_key(const std::string& home_dir) {
    auto tok = load_gemini_token(home_dir);
    if ( tok && !tok->api_key.empty() )
        return tok->api_key;
    return "";
}

GeminiToken login_gemini(api::Client& client, const std::string& home_dir) {
    std::string verifier = random_string(64);
    auto hash = sha256(verifier);
    std::string challenge = base64_url_encode(std::string(
        reinterpret_cast<const char*>(hash.data()), hash.size()));
    std::string state = random_string(32);

    std::string auth_url = std::string(AUTH_URL) + "?" +
        "client_id=" + url_encode(CLIENT_ID) +
        "&redirect_uri=" + url_encode(REDIRECT_URI) +
        "&response_type=code" +
        "&scope=" + url_encode(OAUTH_SCOPES) +
        "&access_type=offline" +
        "&prompt=consent" +
        "&code_challenge=" + url_encode(challenge) +
        "&code_challenge_method=S256" +
        "&state=" + url_encode(state);

    std::cout << "\nGoogle Gemini authentication required.\n\n"
              << "Open this URL in a browser:\n  " << auth_url << "\n\n";

    std::cout << "Paste the authorization code from the browser and press Enter: ";
    std::cout.flush();

    struct termios saved{};
    bool have_tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0;
    if ( have_tty ) {
        struct termios sane = saved;
        sane.c_lflag |= ( ICANON | ECHO | ECHOE | ISIG );
        sane.c_iflag |= ICRNL;
        tcsetattr(STDIN_FILENO, TCSANOW, &sane);
    }

    std::string code;
    bool ok = static_cast<bool>(std::getline(std::cin, code));

    if ( have_tty )
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);

    if ( !ok || common::trim_ws(code).empty() )
        throws << "no authorization code provided" << std::endl;

    code = common::trim_ws(code);

    JSON exchange_body = JSON::Object{
        { "grant_type", "authorization_code" },
        { "client_id", CLIENT_ID },
        { "client_secret", CLIENT_SECRET },
        { "code", code },
        { "code_verifier", verifier },
        { "redirect_uri", REDIRECT_URI }
    };

    std::string response = client.post(TOKEN_URL, "", "", exchange_body.dump_minified());
    JSON reply = JSON::parse(response);
    if ( !reply.contains("access_token") ) {
        std::string err = reply.contains("error") ? reply["error"].to_string() : response;
        throws << "Gemini OAuth login failed: " << err << std::endl;
    }

    GeminiToken token = token_from_json(reply);
    std::int64_t expires_in = 3600;
    if ( reply.contains("expires_in") ) {
        if ( reply["expires_in"] == JSON::TYPE::INT )
            expires_in = static_cast<std::int64_t>(reply["expires_in"]);
        else if ( reply["expires_in"] == JSON::TYPE::FLOAT )
            expires_in = static_cast<std::int64_t>(static_cast<long double>(reply["expires_in"]));
    }
    token.expiry_date_ms = now_ms() + (expires_in * 1000);

    save_gemini_token(home_dir, token);
    return token;
}

} // namespace agent::auth
