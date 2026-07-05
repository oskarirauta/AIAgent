#include "agent/auth/claude_oauth.hpp"

#include <curl/curl.h>
#include <random>
#include <sstream>
#include <iomanip>
#include <array>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <iostream>
#include "json.hpp"
#include "logger.hpp"
#include "throws.hpp"
#include "common.hpp"

namespace agent::auth {

// Subscription (Pro/Max) OAuth flow used by the official claude-code CLI.
// Authorization happens on claude.ai so the issued token is bound to the user's
// Claude subscription (and its inference entitlement) rather than to the
// developer console / pay-as-you-go API billing.
static constexpr const char* OAUTH_AUTHORIZE_URL = "https://claude.ai/oauth/authorize";
static constexpr const char* OAUTH_TOKEN_URL = "https://console.anthropic.com/v1/oauth/token";
static constexpr const char* CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
constexpr const char* MANUAL_REDIRECT_URI = "https://console.anthropic.com/oauth/code/callback";

// Scopes requested by the official claude-code CLI. `user:inference` is what lets
// the access token be used directly against /v1/messages under the subscription;
// without it the token can only mint an API key (which bills API credits).
static constexpr const char* OAUTH_SCOPES = "org:create_api_key user:profile user:inference";

static std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
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

// Simple SHA-256 implementation (no external crypto library required).
static std::array<std::uint8_t, 32> sha256(const std::string& input) {
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
            h = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e; h5 += f; h6 += g; h7 += h;
    }

    std::array<std::uint8_t, 32> out{};
    auto store = [&out](std::uint32_t v, size_t o) {
        out[o] = static_cast<std::uint8_t>(v >> 24);
        out[o + 1] = static_cast<std::uint8_t>(v >> 16);
        out[o + 2] = static_cast<std::uint8_t>(v >> 8);
        out[o + 3] = static_cast<std::uint8_t>(v);
    };
    store(h0, 0); store(h1, 4); store(h2, 8); store(h3, 12);
    store(h4, 16); store(h5, 20); store(h6, 24); store(h7, 28);
    return out;
}

static std::string base64_url_encode(const std::string& input) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for ( unsigned char c : input ) {
        val = (val << 8) + c;
        valb += 8;
        while ( valb >= 0 ) {
            out.push_back(table[(val >> valb) & 0x3f]);
            valb -= 6;
        }
    }
    if ( valb > -6 )
        out.push_back(table[((val << 8) >> (valb + 8)) & 0x3f]);
    for ( auto& c : out ) {
        if ( c == '+' ) c = '-';
        else if ( c == '/' ) c = '_';
    }
    return out;
}

static std::string random_string(size_t length) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    static std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution<size_t> dist(0, sizeof(alphabet) - 2);
    std::string s;
    s.reserve(length);
    for ( size_t i = 0; i < length; ++i )
        s += alphabet[dist(gen)];
    return s;
}

PKCE generate_pkce() {
    PKCE pkce;
    pkce.verifier = random_string(128);

    auto hash = sha256(pkce.verifier);
    pkce.challenge = base64_url_encode(std::string(
        reinterpret_cast<const char*>(hash.data()), hash.size()));
    return pkce;
}

std::string authorization_url(
    const std::string& client_id,
    const std::string& redirect_uri,
    const std::string& state,
    const std::string& code_challenge) {

    return std::string(OAUTH_AUTHORIZE_URL) + "?" +
           "code=true" +
           "&client_id=" + url_encode(client_id) +
           "&response_type=code" +
           "&redirect_uri=" + url_encode(redirect_uri) +
           "&state=" + url_encode(state) +
           "&code_challenge=" + url_encode(code_challenge) +
           "&code_challenge_method=S256" +
           "&scope=" + url_encode(OAUTH_SCOPES);
}

static ClaudeToken token_from_json(const JSON& j) {
    ClaudeToken token;
    if ( j.contains("access_token"))
        token.access_token = j["access_token"].to_string();
    if ( j.contains("refresh_token"))
        token.refresh_token = j["refresh_token"].to_string();
    if ( j.contains("api_key"))
        token.api_key = j["api_key"].to_string();
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

    token.expires_at = now_seconds() + token.expires_in;
    return token;
}

ClaudeToken exchange_code(
    api::Client& client,
    const std::string& /*client_id*/,
    const std::string& code,
    const std::string& code_verifier,
    const std::string& redirect_uri,
    const std::string& state) {

    JSON body = JSON::Object{
        { "grant_type", "authorization_code" },
        { "client_id", CLIENT_ID },
        { "code", code },
        { "code_verifier", code_verifier },
        { "redirect_uri", redirect_uri },
        { "state", state }
    };

    std::string response = client.post(OAUTH_TOKEN_URL, "", "", body.dump_minified());
    JSON j = JSON::parse(response);
    if ( !j.contains("access_token"))
        throws << "token response missing access_token" << std::endl;

    return token_from_json(j);
}

ClaudeToken refresh_access_token(
    api::Client& client,
    const std::string& refresh_token) {

    JSON body = JSON::Object{
        { "grant_type", "refresh_token" },
        { "client_id", CLIENT_ID },
        { "refresh_token", refresh_token }
    };

    std::string response = client.post(OAUTH_TOKEN_URL, "", "", body.dump_minified());
    JSON j = JSON::parse(response);
    if ( !j.contains("access_token"))
        throws << "refresh token response missing access_token" << std::endl;

    return token_from_json(j);
}

std::string create_claude_api_key(
    api::Client& client,
    const std::string& access_token) {

    static constexpr const char* API_KEY_URL = "https://api.anthropic.com/api/oauth/claude_cli/create_api_key";

    std::string response = client.post(API_KEY_URL, "Authorization", "Bearer " + access_token, "null");
    logger::debug["claude"] << "create_api_key response: " << response << std::endl;
    JSON j = JSON::parse(response);
    if ( !j.contains("raw_key"))
        throws << "create_api_key response missing raw_key" << std::endl;

    std::string key = j["raw_key"].to_string();
    logger::debug["claude"] << "create_api_key raw_key prefix: " << key.substr(0, std::min<size_t>(key.size(), 20)) << std::endl;
    return key;
}

std::string prompt_for_authorization_code() {
    std::cout << "Paste the authorization code shown in the browser and press Enter: ";
    std::cout.flush();

    std::string code;
    if ( !std::getline(std::cin, code))
        return "";

    // The official CLI pastes codes in the form "authorizationCode#state".
    // We only need the code part.
    size_t sep = code.find('#');
    if ( sep != std::string::npos )
        code = code.substr(0, sep);

    code = common::trim_ws(code);
    return code;
}

static std::string credentials_dir(const std::string& home_dir) {
    return home_dir + "/credentials";
}

static std::string token_path(const std::string& home_dir) {
    return credentials_dir(home_dir) + "/claude.json";
}

static void ensure_credentials_dir(const std::string& home_dir) {
    std::string dir = credentials_dir(home_dir);
    if ( !std::filesystem::exists(dir))
        std::filesystem::create_directories(dir);
    chmod(dir.c_str(), 0700);
}

std::optional<ClaudeToken> load_claude_token(const std::string& home_dir) {
    std::string path = token_path(home_dir);
    if ( !std::filesystem::exists(path))
        return std::nullopt;

    std::ifstream ifd(path, std::ios::in);
    if ( !ifd.is_open()) {
        logger::warning["claude"] << "failed to open token file: " << path << std::endl;
        return std::nullopt;
    }

    std::stringstream ss;
    ss << ifd.rdbuf();
    std::string raw = ss.str();
    if ( raw.empty())
        return std::nullopt;

    try {
        JSON j = JSON::parse(raw);
        ClaudeToken token;
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
        logger::warning["claude"] << "failed to parse token file: " << e.what() << std::endl;
        return std::nullopt;
    }
}

void save_claude_token(const std::string& home_dir, const ClaudeToken& token) {
    ensure_credentials_dir(home_dir);
    std::string path = token_path(home_dir);
    std::string tmp = path + ".tmp";

    JSON j = JSON::Object{
        { "access_token", token.access_token },
        { "refresh_token", token.refresh_token },
        { "api_key", token.api_key },
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

void remove_claude_token(const std::string& home_dir) {
    std::string path = token_path(home_dir);
    if ( std::filesystem::exists(path))
        std::filesystem::remove(path);
}

bool token_needs_refresh(const ClaudeToken& token, std::int64_t margin_seconds) {
    if ( token.expires_at <= 0 )
        return true;
    return now_seconds() + margin_seconds >= token.expires_at;
}

} // namespace agent::auth
