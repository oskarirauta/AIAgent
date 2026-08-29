#include "agent/api/rate_limit.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include "json.hpp"
#include "common.hpp"

namespace agent::api {

namespace {

std::string lower(const std::string& s) { return common::to_lower(s); }

const std::string* find_header(const std::vector<std::pair<std::string, std::string>>& headers,
                               const std::string& name) {
    for ( const auto& h : headers )
        if ( lower(h.first) == name )
            return &h.second;
    return nullptr;
}

// timegm() without depending on the GNU extension being present: convert a
// broken-down UTC time to a unix timestamp. (mktime interprets local time, which
// would shift every parsed timestamp by the machine's offset.)
std::time_t utc_to_time(std::tm tm) {
    static const int cumulative_days[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    long year = tm.tm_year + 1900;
    long days = ( year - 1970 ) * 365 + (( year - 1969 ) / 4 ) - (( year - 1901 ) / 100 )
              + (( year - 1601 ) / 400 );
    days += cumulative_days[tm.tm_mon % 12];
    // Leap day only counts once the year is past February.
    bool leap = ( year % 4 == 0 && year % 100 != 0 ) || ( year % 400 == 0 );
    if ( leap && tm.tm_mon >= 2 ) days += 1;
    days += tm.tm_mday - 1;
    return static_cast<std::time_t>((( days * 24L + tm.tm_hour ) * 60L + tm.tm_min ) * 60L + tm.tm_sec );
}

// A Go-style duration as OpenAI returns in x-ratelimit-reset-*: "6m0s", "1.5s",
// "2h10m3s", "150ms". Returns seconds (rounded up), or -1 when unparsable.
long parse_go_duration(const std::string& s) {
    if ( s.empty()) return -1;
    double total = 0.0;
    size_t i = 0;
    bool any = false;
    while ( i < s.size()) {
        size_t start = i;
        while ( i < s.size() && ( std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' )) ++i;
        if ( start == i ) return -1;
        double value = std::atof(s.substr(start, i - start).c_str());
        size_t unit_start = i;
        while ( i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
        std::string unit = s.substr(unit_start, i - unit_start);
        if ( unit == "h" )       total += value * 3600.0;
        else if ( unit == "m" )  total += value * 60.0;
        else if ( unit == "s" )  total += value;
        else if ( unit == "ms" ) total += value / 1000.0;
        else if ( unit == "us" || unit == "µs" ) total += value / 1000000.0;
        else return -1;
        any = true;
    }
    if ( !any ) return -1;
    return static_cast<long>(std::ceil(total));
}

// A bare number that may be either unix-epoch seconds or milliseconds. Values
// far beyond the epoch-second range are treated as milliseconds (OpenRouter
// sends ms), so a reset never lands thousands of years in the future.
bool parse_epoch(const std::string& s, std::time_t& out) {
    if ( s.empty()) return false;
    for ( char c : s )
        if ( !std::isdigit(static_cast<unsigned char>(c))) return false;
    errno = 0;
    long long v = std::strtoll(s.c_str(), nullptr, 10);
    if ( v <= 0 ) return false;
    if ( v > 100000000000LL ) v /= 1000; // milliseconds
    out = static_cast<std::time_t>(v);
    return true;
}

} // namespace

bool parse_rfc3339(const std::string& s, std::time_t& out) {
    // 2026-08-28T18:32:00Z | 2026-08-28T18:32:00.123456Z | 2026-08-28T18:32:00+03:00
    if ( s.size() < 19 ) return false;
    std::tm tm{};
    int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
    if ( std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &mon, &day, &hour, &min, &sec) != 6 )
        return false;
    if ( mon < 1 || mon > 12 || day < 1 || day > 31 ) return false;
    tm.tm_year = year - 1900;
    tm.tm_mon  = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min  = min;
    tm.tm_sec  = sec;
    std::time_t t = utc_to_time(tm);

    // Trailing zone: Z (UTC) or ±HH:MM, which must be subtracted to reach UTC.
    size_t pos = 19;
    if ( pos < s.size() && s[pos] == '.' ) {          // skip fractional seconds
        ++pos;
        while ( pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
    }
    if ( pos < s.size() && ( s[pos] == '+' || s[pos] == '-' )) {
        int oh = 0, om = 0;
        if ( std::sscanf(s.c_str() + pos + 1, "%2d:%2d", &oh, &om) == 2 ) {
            long offset = ( oh * 3600L + om * 60L ) * ( s[pos] == '-' ? -1 : 1 );
            t -= offset;
        }
    }
    out = t;
    return true;
}

std::string human_duration(long seconds) {
    if ( seconds <= 0 ) return "now";
    if ( seconds < 60 ) return std::to_string(seconds) + "s";
    if ( seconds < 3600 ) {
        long m = seconds / 60, s = seconds % 60;
        return s ? std::to_string(m) + "m " + std::to_string(s) + "s"
                 : std::to_string(m) + "m";
    }
    if ( seconds < 86400 ) {
        long h = seconds / 3600, m = ( seconds % 3600 ) / 60;
        return m ? std::to_string(h) + "h " + std::to_string(m) + "m"
                 : std::to_string(h) + "h";
    }
    long d = seconds / 86400, h = ( seconds % 86400 ) / 3600;
    return h ? std::to_string(d) + "d " + std::to_string(h) + "h"
             : std::to_string(d) + "d";
}

RateLimitInfo parse_rate_limit(const std::vector<std::pair<std::string, std::string>>& headers,
                               const std::string& body,
                               std::time_t now) {
    if ( now == 0 ) now = std::time(nullptr);
    RateLimitInfo info;
    info.kind = RateLimitInfo::Kind::Rate;

    // The monthly spend cap is a 429 that retrying cannot fix, and Anthropic
    // marks it explicitly. Detect it first so we never tell the user to "wait".
    std::string lowered_body = lower(body);
    if ( lowered_body.find("enforced_spend_limit_reached") != std::string::npos ||
         lowered_body.find("monthly api usage threshold") != std::string::npos ||
         lowered_body.find("reached your api usage limits") != std::string::npos ||
         lowered_body.find("reached your specified api usage limits") != std::string::npos ) {
        info.kind = RateLimitInfo::Kind::SpendCap;
    }

    // A subscription/session quota (Claude Code style) rather than a per-minute
    // rate: the unified header is only sent for the session bucket.
    if ( find_header(headers, "anthropic-ratelimit-unified-reset") ||
         find_header(headers, "anthropic-ratelimit-unified-status") ||
         lowered_body.find("session limit") != std::string::npos ||
         lowered_body.find("usage limit") != std::string::npos ) {
        if ( info.kind != RateLimitInfo::Kind::SpendCap )
            info.kind = RateLimitInfo::Kind::Session;
    }

    // --- When does it clear? Most specific source first. -------------------

    // retry-after: seconds (per RFC also an HTTP-date, which curl gives verbatim).
    if ( const std::string* ra = find_header(headers, "retry-after")) {
        std::string v = common::trim_ws(*ra);
        bool numeric = !v.empty() &&
                       std::all_of(v.begin(), v.end(),
                                   [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        if ( numeric ) {
            info.reset_at = now + std::atol(v.c_str());
            info.has_reset = true;
        } else {
            std::time_t t = 0;
            if ( parse_rfc3339(v, t)) { info.reset_at = t; info.has_reset = true; }
        }
    }

    // Anthropic's unified (session) reset is unix epoch seconds.
    if ( !info.has_reset ) {
        if ( const std::string* u = find_header(headers, "anthropic-ratelimit-unified-reset")) {
            std::time_t t = 0;
            if ( parse_epoch(common::trim_ws(*u), t)) { info.reset_at = t; info.has_reset = true; }
        }
    }

    // Per-bucket RFC 3339 resets. Prefer whichever bucket is actually exhausted
    // (remaining == 0); otherwise take the soonest, since that is when the
    // request could next succeed.
    if ( !info.has_reset ) {
        struct Bucket { const char* reset; const char* remaining; const char* label; };
        static const Bucket buckets[] = {
            { "anthropic-ratelimit-input-tokens-reset",  "anthropic-ratelimit-input-tokens-remaining",  "input tokens" },
            { "anthropic-ratelimit-output-tokens-reset", "anthropic-ratelimit-output-tokens-remaining", "output tokens" },
            { "anthropic-ratelimit-tokens-reset",        "anthropic-ratelimit-tokens-remaining",        "tokens" },
            { "anthropic-ratelimit-requests-reset",      "anthropic-ratelimit-requests-remaining",      "requests" },
        };
        std::time_t soonest = 0; std::string soonest_scope;
        for ( const auto& b : buckets ) {
            const std::string* rv = find_header(headers, b.reset);
            if ( !rv ) continue;
            std::time_t t = 0;
            if ( !parse_rfc3339(common::trim_ws(*rv), t)) continue;

            const std::string* rem = find_header(headers, b.remaining);
            bool exhausted = rem && std::atol(common::trim_ws(*rem).c_str()) <= 0;
            if ( exhausted ) { // the bucket that actually blocked the request
                info.reset_at = t; info.has_reset = true; info.scope = b.label;
                break;
            }
            if ( soonest == 0 || t < soonest ) { soonest = t; soonest_scope = b.label; }
        }
        if ( !info.has_reset && soonest != 0 ) {
            info.reset_at = soonest; info.has_reset = true; info.scope = soonest_scope;
        }
    }

    // OpenRouter / OpenAI-compatible: x-ratelimit-reset is an absolute epoch,
    // while OpenAI's x-ratelimit-reset-{requests,tokens} are durations.
    if ( !info.has_reset ) {
        if ( const std::string* xr = find_header(headers, "x-ratelimit-reset")) {
            std::time_t t = 0;
            if ( parse_epoch(common::trim_ws(*xr), t)) { info.reset_at = t; info.has_reset = true; }
        }
    }
    if ( !info.has_reset ) {
        static const std::pair<const char*, const char*> dur[] = {
            { "x-ratelimit-reset-tokens",   "tokens" },
            { "x-ratelimit-reset-requests", "requests" },
        };
        long best = -1; std::string best_scope;
        for ( const auto& d : dur ) {
            const std::string* v = find_header(headers, d.first);
            if ( !v ) continue;
            long secs = parse_go_duration(common::trim_ws(*v));
            if ( secs < 0 ) continue;
            if ( best < 0 || secs < best ) { best = secs; best_scope = d.second; }
        }
        if ( best >= 0 ) {
            info.reset_at = now + best; info.has_reset = true;
            if ( info.scope.empty()) info.scope = best_scope;
        }
    }

    if ( info.has_reset ) {
        long delta = static_cast<long>(info.reset_at - now);
        info.seconds_until = delta > 0 ? delta : 0;
    }
    return info;
}

std::string format_reset_time(const RateLimitInfo& info) {
    if ( !info.has_reset ) return "";
    std::string out;
    char buf[64];
    std::tm tm{};
    std::time_t t = info.reset_at;
    if ( localtime_r(&t, &tm) && std::strftime(buf, sizeof(buf), "%H:%M", &tm) > 0 )
        out = buf;
    if ( info.seconds_until > 0 ) {
        std::string rel = "in " + human_duration(info.seconds_until);
        out = out.empty() ? rel : out + " (" + rel + ")";
    }
    return out;
}

std::string describe_rate_limit(const RateLimitInfo& info) {
    if ( !info.valid()) return "";

    std::string out;
    switch ( info.kind ) {
        case RateLimitInfo::Kind::SpendCap:
            out = "The usage limit has been reached.";
            break;
        case RateLimitInfo::Kind::Session:
            out = "The usage limit has been reached.";
            break;
        default:
            out = "The request rate limit has been reached.";
            break;
    }

    if ( info.has_reset ) {
        std::string when = format_reset_time(info);
        if ( !when.empty()) out += " Resets at " + when + ".";
    } else if ( info.kind == RateLimitInfo::Kind::SpendCap ) {
        out += " Retrying will not help until the cap resets or is raised.";
    } else {
        out += " The provider did not say when it resets.";
    }

    return out;
}

} // namespace agent::api
