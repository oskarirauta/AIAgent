#pragma once

#include <string>
#include <vector>
#include <utility>
#include <ctime>

namespace agent::api {

// What a 429 (or a quota-carrying success) actually means, decoded from the
// provider's response headers and error body.
//
// Providers express "you are cut off until X" in several incompatible ways:
//   - retry-after: seconds to wait (or an HTTP-date)
//   - anthropic-ratelimit-*-reset: RFC 3339 timestamps
//   - anthropic-ratelimit-unified-reset: unix epoch seconds (subscription/session)
//   - x-ratelimit-reset: unix epoch, seconds or milliseconds (OpenRouter)
//   - x-ratelimit-reset-requests/-tokens: Go-style durations, e.g. "6m0s" (OpenAI)
// This normalises all of them to one absolute instant so the UI can say when
// access comes back instead of "wait a moment".
struct RateLimitInfo {
    enum class Kind {
        None,       // not a quota problem
        Rate,       // ordinary per-minute rate limit — retry shortly
        Session,    // subscription/session quota (Claude) — resets on a longer cycle
        SpendCap    // monthly spend cap reached — retrying cannot help
    };

    Kind kind = Kind::None;
    bool has_reset = false;     // a reset instant was determined
    std::time_t reset_at = 0;   // absolute wall-clock time access returns
    long seconds_until = 0;     // reset_at - now, clamped at 0
    std::string scope;          // which limit was hit ("input tokens", "requests", …)

    bool valid() const { return kind != Kind::None; }
};

// Decode the rate-limit state. `now` defaults to the current time; pass an
// explicit value to keep tests deterministic.
RateLimitInfo parse_rate_limit(const std::vector<std::pair<std::string, std::string>>& headers,
                               const std::string& body,
                               std::time_t now = 0);

// One human sentence: what happened and when it clears. Empty when there is
// nothing useful to say.
std::string describe_rate_limit(const RateLimitInfo& info);

// Just the reset instant, e.g. "20:15 (in 2h 15m)". Empty when unknown. Used to
// report remaining quota on a successful response, where nothing has been hit
// and describe_rate_limit()'s "you've hit ..." phrasing would be wrong.
std::string format_reset_time(const RateLimitInfo& info);

// "45s", "12m 30s", "2h 5m" — a compact, readable duration.
std::string human_duration(long seconds);

// Parse an RFC 3339 / ISO 8601 timestamp ("2026-08-28T18:32:00Z", with an
// optional fractional part and ±HH:MM offset) to a unix timestamp.
bool parse_rfc3339(const std::string& s, std::time_t& out);

} // namespace agent::api
