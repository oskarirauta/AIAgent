#include "agent/config.hpp"

#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include "common.hpp"
#include "logger.hpp"
#include "agent/theme.hpp" // colour-spec validation for the `theme.<role>` keys

namespace agent {

static std::string trim(const std::string& s) {
    return common::trim_ws(s);
}

static std::string model_annotation_1m = "[1m]";

static std::string strip_model_annotation(const std::string& model, bool* has_1m = nullptr) {
    std::string out = trim(model);
    if ( out.size() >= model_annotation_1m.size() ) {
        std::string tail = out.substr(out.size() - model_annotation_1m.size());
        for ( char& c : tail )
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if ( tail == model_annotation_1m ) {
            if ( has_1m ) *has_1m = true;
            return trim(out.substr(0, out.size() - model_annotation_1m.size()));
        }
    }
    if ( has_1m ) *has_1m = false;
    return out;
}

// One place for boolean-ish config values so every flag accepts the same set
// (true/1/yes/on) — previously some keys silently rejected "on".
static bool parse_bool(const std::string& value) {
    std::string v = common::to_lower(common::trim_ws(value));
    return v == "true" || v == "1" || v == "yes" || v == "on";
}

// A comma-separated list value ("a, b, c") — trimmed, empties dropped.
static std::vector<std::string> parse_list(const std::string& value) {
    std::vector<std::string> out;
    std::istringstream is(value);
    std::string item;
    while ( std::getline(is, item, ',')) {
        item = common::trim_ws(item);
        if ( !item.empty())
            out.push_back(item);
    }
    return out;
}

// Parse an unsigned integer setting, keeping the current value and warning on
// malformed input instead of letting std::stoull throw and crash the program.
static size_t parse_size(const std::string& value, size_t current, const std::string& key) {
    try {
        std::string trimmed = common::trim_ws(value);
        // std::stoull happily wraps a leading "-" into a huge value; reject it.
        if ( !trimmed.empty() && trimmed[0] == '-' )
            throw std::invalid_argument("negative");
        size_t idx = 0;
        size_t n = std::stoull(trimmed, &idx);
        std::string suffix = common::trim_ws(trimmed.substr(idx));
        if ( !suffix.empty()) {
            char c = suffix[0];
            if ( c == 'k' || c == 'K' ) n *= 1024;
            else if ( c == 'm' || c == 'M' ) n *= 1024 * 1024;
            else if ( c == 'g' || c == 'G' ) n *= 1024ull * 1024 * 1024;
        }
        return n;
    } catch ( const std::exception& ) {
        logger::warning["config"] << "invalid numeric value for " << key << ": '" << value
                                  << "' (keeping " << current << ")" << std::endl;
        return current;
    }
}

// Token-valued config keys use the decimal parser ("64K" = 64000), matching what
// the UI prints and accepts. Byte-ish keys keep the 1024-based parse_size above.
static size_t parse_token_value(const std::string& value, size_t current, const std::string& key) {
    size_t n = Config::parse_tokens(value, current);
    if ( n == current && !common::trim_ws(value).empty()) {
        // parse_tokens() returns the fallback on bad input; warn like parse_size.
        std::string t = common::trim_ws(value);
        bool numeric = !t.empty() && ( std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '.' );
        if ( !numeric )
            logger::warning["config"] << "invalid token value for " << key << ": '" << value
                                      << "' (keeping " << current << ")" << std::endl;
    }
    return n;
}

size_t Config::parse_size_suffixed(const std::string& value, size_t fallback) {
    return parse_size(value, fallback, "value");
}

static std::string parse_value(const std::string& raw) {
    std::string s = raw;
    s = trim(s);
    if ( s.size() >= 2 && s.front() == '"' && s.back() == '"' ) {
        s = s.substr(1, s.size() - 2);
        // unescape simple escapes
        std::string out;
        for ( size_t i = 0; i < s.size(); i++ ) {
            if ( s[i] == '\\' && i + 1 < s.size()) {
                char next = s[i+1];
                if ( next == 'n' ) out += '\n';
                else if ( next == 't' ) out += '\t';
                else if ( next == 'r' ) out += '\r';
                else out += next;
                i++;
            } else out += s[i];
        }
        return out;
    }
    return s;
}

std::string Config::expand_tilde(const std::string& path) {
    if ( path.empty() || path[0] != '~' )
        return path;
    const char* home = std::getenv("HOME");
    if ( !home || !*home )
        home = "/root";
    if ( path.size() == 1 )                 // "~"
        return home;
    if ( path[1] == '/' )                   // "~/..."
        return std::string(home) + path.substr(1);
    return path;                            // "~user" is not expanded
}

std::string Config::default_path() {
    // The config file lives in the data directory alongside conversations,
    // credentials and memory, so one directory holds everything. A backup or
    // migration that copies the data dir then cannot leave the config behind --
    // and, more importantly, cannot miss the irreplaceable conversation history
    // by glancing only at a separate ~/.config location.
    return default_home_dir() + "/config";
}

std::string Config::default_model_for(const std::string& provider) {
    if ( provider == "codex" ) return "gpt-5.6";
    if ( provider == "claude" ) return "claude-opus-4-8";
    if ( provider == "anthropic" ) return "claude-opus-4-8";
    if ( provider == "gemini" ) return "gemini-3.6-flash";
    if ( provider == "kimi" ) return "kimi-for-coding";     // managed:kimi-code / "K2.7 Code"
    if ( provider == "moonshot" ) return "kimi-k2-0905-preview";
    if ( provider == "openrouter" ) return "openrouter/free"; // auto-routes to an available free model; -m for a specific one
    if ( provider == "ollama" ) return "llama3";
    return "gpt-4o-mini"; // openai and any other OpenAI-compatible provider
}

std::string Config::base_model_name(const std::string& model) {
    return strip_model_annotation(model);
}

bool Config::model_requests_1m_context(const std::string& model) {
    bool requested = false;
    (void)strip_model_annotation(model, &requested);
    return requested;
}

// Well-known models per provider, best/most-capable first. Only providers with a
// closed, curated namespace are listed: Ollama's models are whatever the user
// pulled locally, and OpenRouter/OpenAI-compatible endpoints are an open
// namespace, so for those we rely on the provider's live listing instead.
const std::vector<std::string>& Config::known_models_for(const std::string& provider) {
    static const std::vector<std::string> anthropic_models = {
        "claude-opus-5",
        "claude-opus-4-8",
        "claude-sonnet-4-6",
        "claude-haiku-4-5-20251001",
        "claude-fable-5",
    };
    static const std::vector<std::string> moonshot_models = {
        "kimi-k2-0905-preview",
        "kimi-k2-turbo-preview",
        "kimi-k2-0711-preview",
        "moonshot-v1-128k",
        "moonshot-v1-32k",
        "moonshot-v1-8k",
    };
    static const std::vector<std::string> kimi_models = {
        "kimi-for-coding",
    };
    static const std::vector<std::string> openai_models = {
        "gpt-5.6",
        "gpt-5.5",
        "gpt-5.4",
        "gpt-5.4-mini",
        "gpt-4o",
        "gpt-4o-mini",
        "gpt-4.1",
        "gpt-4.1-mini",
        "o3",
        "o3-mini",
        "gpt-4-turbo",
        "gpt-3.5-turbo",
    };
    // ChatGPT-backed Codex exposes an account-scoped catalogue. Keep the known
    // ChatGPT/Codex family in the picker, best/newest first; users can still pass
    // any future/entitled slug explicitly with -m.
    static const std::vector<std::string> codex_models = {
        "gpt-6-astra",
        "gpt-5.6-sol",
        "gpt-5.6-terra",
        "gpt-5.6-luna",
        "gpt-5.6",
        "gpt-5.5",
        "gpt-5.4",
        "gpt-5.4-mini",
    };
    static const std::vector<std::string> gemini_models = {
        "gemini-3.6-flash",
        "gemini-flash-latest",
        "gemini-3.1-pro-preview",
        "gemini-pro-latest",
        "gemini-3.5-flash",
        "gemini-3.1-flash-lite",
        "gemini-2.5-pro",
        "gemini-2.5-flash",
    };
    static const std::vector<std::string> openrouter_models = {
        "openrouter/auto",
        "openrouter/free",
    };
    static const std::vector<std::string> none;

    if ( provider == "claude" || provider == "anthropic" ) return anthropic_models;
    if ( provider == "moonshot" ) return moonshot_models;
    if ( provider == "kimi" ) return kimi_models;
    if ( provider == "openai" ) return openai_models;
    if ( provider == "codex" ) return codex_models;
    if ( provider == "gemini" ) return gemini_models;
    if ( provider == "openrouter" ) return openrouter_models;
    return none; // ollama and anything custom: user-defined namespace
}

namespace {

// Short hand -> canonical *fragment* aliases. The value is matched against the
// candidate list, so the table stays valid when a model generation is bumped
// (e.g. "opus" keeps resolving after claude-opus-4-8 becomes -4-9).
struct ModelAlias { const char* from; const char* to; };
static const ModelAlias model_aliases[] = {
    // Anthropic families
    { "opus",    "claude-opus" },
    { "sonnet",  "claude-sonnet" },
    { "haiku",   "claude-haiku" },
    { "fable",   "claude-fable" },
    // Moonshot / Kimi
    { "k2",      "kimi-k2" },
    { "kimi",    "kimi" },
    { "turbo",   "kimi-k2-turbo" },
    // OpenAI
    { "4o",      "gpt-4o" },
    { "4o-mini", "gpt-4o-mini" },
    { "mini",    "gpt-4o-mini" },
    { "4.1",     "gpt-4.1" },
    { "gpt4",    "gpt-4o" },
    { "gpt",     "gpt-4o" },
    { "3.5",     "gpt-3.5-turbo" },
    // Codex / ChatGPT Desktop
    { "astra",   "gpt-6-astra" },
    { "sol",     "gpt-5.6-sol" },
    { "terra",   "gpt-5.6-terra" },
    { "luna",    "gpt-5.6-luna" },
    // OpenRouter
    { "auto",    "openrouter/auto" },
    { "free",    "openrouter/free" },
    // Google Gemini
    { "gemini",  "gemini-3.6-flash" },
    { "flash",   "gemini-3.6-flash" },
    { "pro",     "gemini-3.1-pro" },
};

// Normalise for comparison: lower-case, and drop the separators people vary on
// ("claude_opus 4.8" and "claude-opus-4-8" must compare equal).
std::string normalize_model(const std::string& s) {
    std::string out;
    for ( char c : s ) {
        if ( c == '-' || c == '_' || c == ' ' || c == '.' || c == '/' || c == ':' )
            continue;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Levenshtein distance, capped: once every cell of a row exceeds `max_distance`
// the result can only grow, so bail out early instead of filling the matrix.
size_t edit_distance(const std::string& a, const std::string& b, size_t max_distance) {
    if ( a.empty()) return b.size();
    if ( b.empty()) return a.size();
    if ( a.size() > b.size() + max_distance || b.size() > a.size() + max_distance )
        return max_distance + 1;

    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for ( size_t j = 0; j <= b.size(); ++j ) prev[j] = j;

    for ( size_t i = 1; i <= a.size(); ++i ) {
        cur[0] = i;
        size_t row_min = cur[0];
        for ( size_t j = 1; j <= b.size(); ++j ) {
            size_t cost = ( a[i-1] == b[j-1] ) ? 0 : 1;
            cur[j] = std::min({ prev[j] + 1, cur[j-1] + 1, prev[j-1] + cost });
            row_min = std::min(row_min, cur[j]);
        }
        if ( row_min > max_distance )
            return max_distance + 1;
        prev.swap(cur);
    }
    return prev[b.size()];
}

// The digits of a model name, in order ("claude-opus-4-8" -> "48"). Digits carry
// the *version*, letters the *family*, so they are compared separately: a curated
// list inevitably goes stale, and a new release must never be "corrected" into
// the older model it is one digit away from.
std::string version_digits(const std::string& s) {
    std::string out;
    for ( char c : s )
        if ( std::isdigit(static_cast<unsigned char>(c))) out += c;
    return out;
}

// How well `input` matches `candidate`, higher is better; 0 = no match at all.
// The tiers are deliberately far apart so a weaker kind of match can never
// outrank a stronger one, and the score within a tier prefers the shortest
// candidate (the least "extra" beyond what the user typed).
size_t match_score(const std::string& input, const std::string& candidate) {
    std::string in = normalize_model(input);
    std::string cand = normalize_model(candidate);
    if ( in.empty() || cand.empty()) return 0;

    auto tighter = [&cand](size_t base) -> size_t {
        // Within a tier, shorter candidates win (bounded so it never leaks
        // into the tier below).
        return base + ( cand.size() < 100 ? 100 - cand.size() : 0 );
    };

    if ( in == cand ) return tighter(4000);
    if ( cand.rfind(in, 0) == 0 ) return tighter(3000);          // prefix: "claudeopus" -> "claudeopus48"
    if ( cand.find(in) != std::string::npos ) return tighter(2000); // substring: "fable" -> "claudefable5"

    // Fuzzy matching is for typos, not for versions. If the user spelled out a
    // version that differs from the candidate's, they meant a different model:
    // "claude-opus-5" must stay itself and reach the API, not be rewritten to
    // "claude-opus-4-8" just because it is two edits away. An input with no
    // digits at all ("sonet") is a family shorthand and stays fuzzy-matchable.
    std::string in_digits = version_digits(input);
    if ( !in_digits.empty() && in_digits != version_digits(candidate))
        return 0;

    // Fuzzy: allow roughly one typo per four characters, at least one, so short
    // names do not collapse into each other ("opus" must not match "haiku").
    size_t budget = std::max<size_t>(1, in.size() / 4);
    size_t best = edit_distance(in, cand, budget);
    if ( best <= budget )
        return tighter(1000 - best * 10);

    // Fuzzy against the candidate's own segments, so a typo in one part of a
    // longer name still lands ("sonet" -> "claude-sonnet-4-6").
    std::string lowered = common::to_lower(candidate);
    std::string segment;
    for ( size_t i = 0; i <= lowered.size(); ++i ) {
        char c = ( i < lowered.size()) ? lowered[i] : '-';
        if ( c == '-' || c == '_' || c == '.' || c == '/' || c == ' ' || c == ':' ) {
            if ( segment.size() >= 3 ) {
                size_t d = edit_distance(in, segment, budget);
                if ( d <= budget )
                    return tighter(1000 - d * 10);
            }
            segment.clear();
        } else segment += c;
    }
    return 0;
}

} // namespace

Config::ModelMatch Config::resolve_model(const std::string& provider, const std::string& input,
                                         const std::vector<std::string>& candidates) {
    ModelMatch result;
    result.model = common::trim_ws(input);
    if ( result.model.empty())
        return result;

    bool wants_1m = false;
    std::string suffixless = strip_model_annotation(result.model, &wants_1m);

    // Prefer a live listing when the caller has one; fall back to the curated set.
    const std::vector<std::string>& known =
        !candidates.empty() ? candidates : known_models_for(provider);
    if ( known.empty())
        return result; // nothing to match against (ollama, custom endpoints)

    // An exact name always wins: never rewrite something that already works.
    for ( const auto& k : known )
        if ( k == suffixless ) {
            result.model = wants_1m ? k + "[1m]" : k;
            return result;
        }

    // An alias expands the input before matching, so "fable" and "opus" become
    // the canonical family fragment and then match the real name below.
    std::string needle = suffixless;
    std::string norm_in = normalize_model(needle);
    for ( const auto& alias : model_aliases ) {
        if ( normalize_model(alias.from) == norm_in ) {
            needle = alias.to;
            break;
        }
    }

    // Score every candidate; ties keep the curated order (best/newest first).
    auto score_all = [&known](const std::string& n) {
        std::vector<std::pair<size_t, std::string>> out;
        for ( const auto& k : known ) {
            size_t score = match_score(n, k);
            if ( score > 0 )
                out.push_back({ score, k });
        }
        return out;
    };
    std::vector<std::pair<size_t, std::string>> scored = score_all(needle);

    // The alias table is global, so an expansion can name a family this provider
    // does not offer ("mini" -> gpt-4o-mini, which no Codex model matches). An
    // alias must only ever help: when it matches nothing, fall back to what the
    // user actually typed rather than letting the expansion bury it.
    if ( scored.empty() && needle != suffixless )
        scored = score_all(suffixless);

    if ( scored.empty())
        return result; // unknown: pass it through untouched, the API decides

    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });

    result.model = scored.front().second;
    if ( wants_1m )
        result.model += "[1m]";
    result.corrected = ( result.model != common::trim_ws(input));
    for ( size_t i = 1; i < scored.size() && i <= 3; ++i )
        result.alternatives.push_back(scored[i].second);
    return result;
}

// Format a token count the way people read it: 8192 -> "8.2K", 200000 -> "200K",
// 1500000 -> "1.5M". Large raw digit strings are hard to compare at a glance
// ("128000" vs "1280000"), which is exactly where budget mistakes happen.
std::string Config::format_tokens(size_t n) {
    auto strip = [](std::string s) {
        // "1.0M" reads worse than "1M"; drop a trailing ".0".
        if ( s.size() > 2 && s[s.size()-2] == '.' && s[s.size()-1] == '0' )
            s.erase(s.size() - 2);
        return s;
    };
    char buf[32];
    if ( n >= 1000000 ) {
        std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(n) / 1000000.0);
        return strip(buf) + "M";
    }
    if ( n >= 1000 ) {
        // Whole thousands print without a decimal: 200000 -> "200K", not "200.0K".
        if ( n % 1000 == 0 )
            return std::to_string(n / 1000) + "K";
        std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(n) / 1000.0);
        return strip(buf) + "K";
    }
    return std::to_string(n);
}

// Parse a token count, accepting the same shorthand format_tokens() prints:
// "32000", "32K", "1.5M". Token counts are decimal, so K is 1000 (not 1024) --
// a user asking for "100K tokens" means 100000, and a 1024-based reading would
// quietly hand them 102400.
size_t Config::parse_tokens(const std::string& value, size_t fallback) {
    std::string s = common::trim_ws(value);
    if ( s.empty()) return fallback;
    if ( s[0] == '-' ) return fallback; // no negative budgets
    try {
        size_t idx = 0;
        double n = std::stod(s, &idx);
        if ( n < 0 ) return fallback;
        std::string suffix = common::trim_ws(s.substr(idx));
        if ( !suffix.empty()) {
            char c = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[0])));
            if ( c == 'k' ) n *= 1000.0;
            else if ( c == 'm' ) n *= 1000000.0;
            else if ( c != 't' ) return fallback; // allow a trailing "tokens"
        }
        if ( n < 0 ) return fallback;
        return static_cast<size_t>(n);
    } catch ( const std::exception& ) {
        return fallback;
    }
}

size_t Config::parse_tool_limit(const std::string& value, size_t fallback) {
    std::string s = common::to_lower(common::trim_ws(value));
    if ( s == "unlimited" || s == "all" )
        return 0;
    return parse_tokens(value, fallback);
}

std::string Config::default_system_prompt_for(const std::string& provider) {
    if ( provider == "codex" )
        return "You are Codex, OpenAI's coding agent, assisting with software engineering "
               "tasks on a Linux system. Be concise and precise.";
    if ( provider == "kimi" )
        return "You are Kimi, an AI assistant built by Moonshot AI, running as a "
               "command-line coding assistant. You help with software engineering "
               "tasks on a Linux system. Be concise and precise.";
    if ( provider == "claude" )
        // The Claude provider additionally injects the required Claude Code
        // identity block, so this second block only sets task context.
        return "You are Claude Code, Anthropic's official CLI, assisting with "
               "software engineering tasks on a Linux system. Be concise and precise.";
    if ( provider == "gemini" )
        return "You are Gemini, Google's coding agent, assisting with software engineering "
               "tasks on a Linux system. Be concise and precise.";
    return "You are a helpful Linux CLI assistant.";
}

size_t Config::context_window_for(const std::string& model) {
    bool wants_1m = false;
    std::string m = common::to_lower(strip_model_annotation(model, &wants_1m));
    auto has = [&](const char* p) { return m.find(p) != std::string::npos; };
    // Explicit 1M-context variants first.
    if ( wants_1m ) return 1000000;
    if ( has("gemini-1.5-pro") || has("gemini-2.5-pro") || has("gemini-3.1-pro") || has("gemini-pro") )
        return 2000000;
    if ( has("gemini") )
        return 1000000;
    if ( has("claude-opus-5") || has("claude-sonnet-5") || has("claude-fable-5") || has("claude-mythos-5") )
        return 1000000;
    if ( has("claude") || has("opus") || has("sonnet") || has("haiku") || has("fable") )
        return 200000;
    if ( has("gpt-6") || has("astra") || has("sol") || has("terra") || has("luna") )
        return 272000;
    if ( has("kimi") || has("moonshot") ) return 256000;
    if ( has("gpt-5.6") ) return 1050000;
    if ( has("gpt-5.5") ) return 1000000;
    if ( has("gpt-5.4-mini") ) return 400000;
    if ( has("gpt-5.4") ) return 1050000;
    if ( has("codex") ) return 400000;
    if ( has("gpt-4o") || has("gpt-4.1") || has("o1") || has("o3") || has("gpt-4-turbo") )
        return 128000;
    if ( has("gpt-4") ) return 128000;
    if ( has("gpt-3.5") ) return 16000;
    if ( has("llama") || has("mistral") || has("qwen") || has("gemma") || has("phi") )
        return 8192;
    return 0; // unknown
}

std::optional<ModelPricing> Config::pricing_for(const std::string& model) const {
    auto it = pricing.find(model);
    if ( it != pricing.end())
        return it->second;
    for ( const auto& [key, price] : pricing )
        if ( !key.empty() && model.find(key) != std::string::npos )
            return price;
    return std::nullopt;
}

Config::ProviderPricingRules Config::provider_pricing_rules(const std::string& prov, const std::string& mdl) const {
    ProviderPricingRules rules;
    std::string p = common::to_lower(common::trim_ws(prov));
    std::string m = common::to_lower(common::trim_ws(mdl));

    // Check if the model pricing has an explicit override first
    if ( auto mp = pricing_for(mdl) ) {
        if ( mp->cache_read_ratio > 0.0 )
            rules.cache_read_ratio = mp->cache_read_ratio;
        if ( mp->cache_write_ratio > 0.0 )
            rules.cache_write_ratio = mp->cache_write_ratio;
        if ( mp->cache_read_ratio > 0.0 || mp->cache_write_ratio > 0.0 )
            return rules;
    }

    // Provider / family defaults:
    if ( p == "openai" || p == "codex" || m.find("gpt-") != std::string::npos || m.find("o1") != std::string::npos || m.find("o3") != std::string::npos ) {
        rules.cache_read_ratio = 0.50; // OpenAI prompt caching is 50% of input rate
        rules.cache_write_ratio = 1.00;
    } else if ( p == "gemini" || m.find("gemini") != std::string::npos ) {
        rules.cache_read_ratio = 0.25; // Gemini context caching is 25% of input rate (75% discount)
        rules.cache_write_ratio = 1.00;
    } else if ( p == "claude" || p == "anthropic" || m.find("claude") != std::string::npos ) {
        rules.cache_read_ratio = 0.10; // Anthropic cache read is 10%
        rules.cache_write_ratio = 1.25; // Anthropic cache creation is 125%
    } else if ( p == "kimi" || p == "moonshot" || m.find("moonshot") != std::string::npos || m.find("kimi") != std::string::npos ) {
        rules.cache_read_ratio = 0.20; // Kimi context caching is ~20%
        rules.cache_write_ratio = 1.00;
    } else if ( m.find("deepseek") != std::string::npos ) {
        rules.cache_read_ratio = 0.25; // DeepSeek prompt cache hit is ~25%
        rules.cache_write_ratio = 1.00;
    } else if ( p == "ollama" ) {
        rules.cache_read_ratio = 0.0;
        rules.cache_write_ratio = 0.0;
    } else {
        rules.cache_read_ratio = 0.10;
        rules.cache_write_ratio = 1.00;
    }

    return rules;
}

double Config::session_cost(long input_tokens, long output_tokens, long cached_input, long cache_creation) const {
    auto p = pricing_for(model);
    if ( !p )
        return -1.0;

    auto rules = provider_pricing_rules(provider, model);
    double read_ratio = ( p->cache_read_ratio > 0.0 ) ? p->cache_read_ratio : rules.cache_read_ratio;
    double write_ratio = ( p->cache_write_ratio > 0.0 ) ? p->cache_write_ratio : rules.cache_write_ratio;

    long full_input = input_tokens - cached_input - cache_creation;
    if ( full_input < 0 ) full_input = std::max(0L, input_tokens - cached_input);

    return static_cast<double>(full_input) / 1e6 * p->input_per_mtok +
           static_cast<double>(cached_input) / 1e6 * p->input_per_mtok * read_ratio +
           static_cast<double>(cache_creation) / 1e6 * p->input_per_mtok * write_ratio +
           static_cast<double>(output_tokens) / 1e6 * p->output_per_mtok;
}

size_t Config::context_budget() const {
    if ( context_auto ) {
        size_t w = context_window_for(model);
        if ( w == 0 )
            return 0;                                       // unknown model: no trimming
        return static_cast<size_t>(w * 0.85);               // leave headroom for the reply
    }
    return context_limit;
}

// The budget auto-compaction measures against. Normally the configured context
// budget — but when the context is "unlimited" there is still a hard limit: the
// model's own window. Falling back to it means auto-compact keeps working as a
// safety net instead of silently doing nothing in exactly the long sessions it
// exists for. Returns 0 only when the window is genuinely unknown.
size_t Config::compaction_budget() const {
    size_t budget = context_budget();
    if ( budget > 0 )
        return budget;
    size_t window = context_window_for(model);
    if ( window == 0 )
        return 0; // unknown model: nothing reliable to measure against
    return static_cast<size_t>(window * 0.85);
}

std::string Config::default_home_dir() {
    const char* home = std::getenv("HOME");
    if ( !home || !*home )
        home = "/root";
    return std::string(home) + "/.local/share/ai-agent";
}

void Config::load(const std::string& path) {

    if ( path.empty())
        return;

    if ( !std::filesystem::exists(path)) {
        logger::verbose["config"] << "config file not found: " << path << std::endl;
        return;
    }

    logger::verbose["config"] << "loading config from " << path << std::endl;

    std::ifstream ifd(path, std::ios::in);
    if ( !ifd.is_open()) {
        logger::warning["config"] << "failed to open config: " << path << std::endl;
        return;
    }

    std::string line;
    while ( std::getline(ifd, line)) {
        line = trim(line);
        if ( line.empty() || line.front() == '#' )
            continue;

        size_t pos = line.find(':');
        if ( pos == std::string::npos )
            continue;

        std::string key = trim(line.substr(0, pos));
        std::string value = parse_value(line.substr(pos + 1));

        if ( key == "provider" ) { provider = value; provider_explicit = true; }
        else if ( key == "model" ) { model = value; model_explicit = true; }
        else if ( key == "api_url" ) api_url = value;
        else if ( key == "api_key" ) api_key = value;
        else if ( key == "oauth_host" ) oauth_host = value;
        else if ( key == "oauth_client_id" ) oauth_client_id = value;
        else if ( key == "log_level" ) log_level = value;
        else if ( key == "theme" ) theme = value;
        else if ( key == "theme_base" ) theme_base = value;
        else if ( key.rfind("theme.", 0) == 0 ) {
            // theme.<role>: <256-index | #rrggbb | colour name> — one override for
            // the "custom" theme. Validated here so a typo is reported at load
            // time instead of silently doing nothing.
            std::string role = trim(key.substr(6));
            if ( !theme_role_name_valid(role))
                logger::warning["config"] << "unknown theme role: " << role
                                          << " (roles: " << theme_role_list() << ")" << std::endl;
            else if ( theme_color_sgr(value).empty())
                logger::warning["config"] << "invalid colour for theme." << role << ": " << value
                                          << " (use 0-255, #rrggbb, or a colour name)" << std::endl;
            else
                theme_colors[role] = value;
        }
        else if ( key == "multiline" ) multiline = parse_bool(value);
        else if ( key == "thinking_stream" ) thinking_stream = parse_bool(value);
        else if ( key == "thinking_collapse" ) thinking_collapse = parse_bool(value);
        else if ( key == "system_prompt" ) system_prompt = value;
        else if ( key == "home_dir" ) home_dir = expand_tilde(value);
        else if ( key == "tools_enabled" ) tools_enabled = parse_bool(value);
        else if ( key == "strict" ) strict = parse_bool(value);
        else if ( key == "context_limit" ) {
            if ( common::to_lower(value) == "auto" ) { context_auto = true; }
            else { context_auto = false; context_limit = parse_token_value(value, context_limit, key); }
        }
        else if ( key == "auto_compact" ) auto_compact = parse_bool(value);
        else if ( key == "auto_compact_pct" ) auto_compact_pct = parse_size(value, auto_compact_pct, key);
        else if ( key == "auto_compact_max_tokens" || key == "auto_compact_limit" ) auto_compact_max_tokens = parse_token_value(value, auto_compact_max_tokens, key);
        else if ( key == "workflow_autoresume" ) workflow_autoresume = parse_bool(value);
        else if ( key == "bell" ) bell = common::to_lower(value);
        else if ( key == "supersede_tools" ) supersede_tools = parse_bool(value);
        else if ( key == "redact_secrets" ) redact_secrets = parse_bool(value);
        else if ( key == "failover" ) failover = parse_list(value);
        else if ( key == "tools_safe" ) tools_safe = parse_list(value);
        else if ( key == "tools_danger" ) tools_danger = parse_list(value);
        else if ( key == "max_tokens" ) { max_tokens = parse_token_value(value, max_tokens, key); max_tokens_explicit = true; }
        else if ( key == "tool_call_limit" ) { tool_call_limit = parse_tool_limit(value, tool_call_limit); tool_call_limit_explicit = true; }
        else if ( key == "advisor" ) advisor = parse_bool(value);
        else if ( key == "advisor_model" ) advisor_model = value;
        else if ( key == "budget_tokens" ) budget_tokens = parse_size(value, budget_tokens, key);
        else if ( key == "web_search" ) web_search = parse_bool(value);
        else if ( key == "web_search_url" ) web_search_url = value;
        else if ( key == "prompt_cache" ) prompt_cache = parse_bool(value);
        else if ( key == "parallel_tools" ) parallel_tools = parse_bool(value);
        else if ( key == "tool_profile" || key == "profile" ) tool_profile = common::to_lower(trim(value));
        else if ( key == "steering" ) steering = trim(value);
        else if ( key == "steering_mode" ) steering_mode = common::to_lower(trim(value));
        else if ( key == "mcp_config" ) mcp_config = expand_tilde(value);
        else if ( key == "budget_usd" ) {
            try { budget_usd = std::stod(common::trim_ws(value)); }
            catch ( ... ) { logger::warning["config"] << "invalid budget_usd: " << value << std::endl; }
        }
        else if ( key.rfind("price.", 0) == 0 ) {
            // price.<model>: <input>/<output>[/<cache_read>[/<cache_write>]]  (USD per million tokens, cache ratio 0..1 or absolute USD)
            std::string model_key = trim(key.substr(6));
            std::vector<std::string> parts;
            {
                std::istringstream iss(value);
                std::string item;
                while ( std::getline(iss, item, '/') )
                    parts.push_back(trim(item));
            }
            if ( model_key.empty() || parts.size() < 2 ) {
                logger::warning["config"] << "invalid price entry: " << key << ": " << value << std::endl;
            } else {
                try {
                    ModelPricing p;
                    p.input_per_mtok = std::stod(parts[0]);
                    p.output_per_mtok = std::stod(parts[1]);
                    if ( parts.size() >= 3 && !parts[2].empty() ) {
                        p.cache_read_ratio = std::stod(parts[2]);
                    }
                    if ( parts.size() >= 4 && !parts[3].empty() ) {
                        p.cache_write_ratio = std::stod(parts[3]);
                    }
                    pricing[model_key] = p;
                } catch ( ... ) {
                    logger::warning["config"] << "invalid price numbers: " << value << std::endl;
                }
            }
        }
        else if ( key == "paste_threshold_chars" ) paste_threshold_chars = parse_size(value, paste_threshold_chars, key);
        else if ( key == "paste_threshold_lines" ) paste_threshold_lines = parse_size(value, paste_threshold_lines, key);
        else if ( key == "paste_single_line_chars" ) paste_single_line_chars = parse_size(value, paste_single_line_chars, key);
        else if ( key == "paste_threshold_ms" ) paste_threshold_ms = parse_size(value, paste_threshold_ms, key);
        else if ( key == "paste_preview" ) paste_preview = parse_size(value, paste_preview, key);
        else if ( key.rfind("provider.", 0) == 0 ) {
            // provider.<name>.<key>: value
            size_t first_dot = key.find('.', 0);
            size_t second_dot = key.find('.', first_dot + 1);
            if ( first_dot != std::string::npos && second_dot != std::string::npos ) {
                std::string prov = key.substr(first_dot + 1, second_dot - first_dot - 1);
                std::string opt = key.substr(second_dot + 1);
                provider_options[prov][opt] = value;
            }
        }
        // confirm_tools is intentionally not loaded from config file; it must be
        // requested explicitly on the command line every session for safety.
    }

    if ( home_dir.empty())
        home_dir = default_home_dir();
}

std::string Config::sanitize_session_name(const std::string& name) {
    std::string s = common::trim_ws(name);
    if ( s.empty() || common::to_lower(s) == "default" )
        return ""; // the project's default session
    std::string out;
    for ( char c : s ) {
        unsigned char u = static_cast<unsigned char>(c);
        out += ( std::isalnum(u) || c == '-' || c == '_' ) ? c : '-';
    }
    // Trim leading/trailing separators so a name can never start a hidden file
    // or end in noise, and cap the length so the filename stays sane.
    while ( !out.empty() && ( out.front() == '-' || out.front() == '_' )) out.erase(out.begin());
    while ( !out.empty() && ( out.back() == '-' || out.back() == '_' )) out.pop_back();
    if ( out.size() > 48 )
        out.resize(48);
    return out;
}

void Config::apply_cli(const usage_t& usage) {

    if ( usage["provider"] ) {
        provider = usage["provider"].stringValue();
        provider_explicit = true;
    }
    if ( usage["model"] ) {
        model = usage["model"].stringValue();
        model_explicit = true;
    }
    if ( usage["api_url"] )
        api_url = usage["api_url"].stringValue();
    if ( usage["api_key"] )
        api_key = usage["api_key"].stringValue();
    if ( usage["log_level"] )
        log_level = usage["log_level"].stringValue();
    if ( usage["system_prompt"] )
        system_prompt = usage["system_prompt"].stringValue();
    if ( usage["home_dir"] )
        home_dir = expand_tilde(usage["home_dir"].stringValue());
    if ( usage["no_tools"] )
        tools_enabled = false;
    if ( usage["yes_tools"] )
        confirm_tools = false;
    if ( usage["insecure"] )
        insecure = true;
    if ( usage["no_tools"] || usage["yes_tools"] || usage["insecure"] )
        tool_mode_explicit = true; // an explicit CLI mode wins over saved state
    if ( usage["steal_lock"] )
        steal_lock = true;
    if ( usage["profile"] )
        tool_profile = common::to_lower(usage["profile"].stringValue());
    if ( usage["session"] )
        session_name = sanitize_session_name(usage["session"].stringValue());
    // paste thresholds and oauth host/client id are config-file only (see load()).
}

static std::string state_path(const std::string& home_dir) {
    return home_dir + "/state.json";
}

Config::LastUsed Config::load_last_used(const std::string& home_dir) {
    LastUsed last;
    std::string path = state_path(home_dir);
    if ( !std::filesystem::exists(path))
        return last;

    std::ifstream ifd(path, std::ios::in);
    if ( !ifd.is_open())
        return last;

    std::stringstream ss;
    ss << ifd.rdbuf();
    if ( ss.str().empty())
        return last;

    try {
        JSON j = JSON::parse(ss.str());
        if ( j.contains("provider") && j["provider"] == JSON::TYPE::STRING )
            last.provider = j["provider"].to_string();
        if ( j.contains("models") && j["models"] == JSON::TYPE::OBJECT ) {
            j["models"].for_each([&last](JSON::fe_iterator& it, const JSON& value) {
                if ( it.named() && value == JSON::TYPE::STRING )
                    last.models[it.name()] = value.to_string();
            });
        }
        if ( j.contains("settings") && j["settings"] == JSON::TYPE::OBJECT ) {
            const JSON& s = j["settings"];
            last.has_settings = true;
            if ( s.contains("settings_version") && s["settings_version"] == JSON::TYPE::INT )
                last.settings_version = static_cast<int>(static_cast<long long>(s["settings_version"]));
            if ( s.contains("theme") && s["theme"] == JSON::TYPE::STRING )
                last.theme = s["theme"].to_string();
            if ( s.contains("thinking") && s["thinking"] == JSON::TYPE::STRING )
                last.thinking = s["thinking"].to_string();
            if ( s.contains("multiline") && s["multiline"] == JSON::TYPE::BOOL )
                last.multiline = s["multiline"].to_bool();
            if ( s.contains("thinking_stream") && s["thinking_stream"] == JSON::TYPE::BOOL )
                last.thinking_stream = s["thinking_stream"].to_bool();
            if ( s.contains("thinking_collapse") && s["thinking_collapse"] == JSON::TYPE::BOOL )
                last.thinking_collapse = s["thinking_collapse"].to_bool();
            if ( s.contains("context_auto") && s["context_auto"] == JSON::TYPE::BOOL )
                last.context_auto = s["context_auto"].to_bool();
            if ( s.contains("context_limit") && s["context_limit"] == JSON::TYPE::INT )
                last.context_limit = static_cast<size_t>(static_cast<long long>(s["context_limit"]));
            if ( s.contains("auto_compact") && s["auto_compact"] == JSON::TYPE::BOOL )
                last.auto_compact = s["auto_compact"].to_bool();
            if ( s.contains("auto_compact_max_tokens") && s["auto_compact_max_tokens"] == JSON::TYPE::INT )
                last.auto_compact_max_tokens = static_cast<size_t>(static_cast<long long>(s["auto_compact_max_tokens"]));
            if ( s.contains("workflow_autoresume") && s["workflow_autoresume"] == JSON::TYPE::BOOL )
                last.workflow_autoresume = s["workflow_autoresume"].to_bool();
            if ( s.contains("confirm_tools") && s["confirm_tools"] == JSON::TYPE::BOOL )
                last.confirm_tools = s["confirm_tools"].to_bool();
            if ( s.contains("insecure") && s["insecure"] == JSON::TYPE::BOOL )
                last.insecure = s["insecure"].to_bool();
            if ( s.contains("bell") && s["bell"] == JSON::TYPE::STRING )
                last.bell = s["bell"].to_string();
            if ( s.contains("advisor") && s["advisor"] == JSON::TYPE::BOOL )
                last.advisor = s["advisor"].to_bool();
            if ( s.contains("advisor_model") && s["advisor_model"] == JSON::TYPE::STRING )
                last.advisor_model = s["advisor_model"].to_string();
            if ( s.contains("paste_preview") && s["paste_preview"] == JSON::TYPE::INT )
                last.paste_preview = static_cast<size_t>(static_cast<long long>(s["paste_preview"]));
            // Budgets the user tunes for a big job: these used to be config-file
            // only, so a value set in /settings silently reverted to the default
            // on the next launch.
            if ( s.contains("tool_call_limit") && s["tool_call_limit"] == JSON::TYPE::INT )
                last.tool_call_limit = static_cast<size_t>(static_cast<long long>(s["tool_call_limit"]));
            if ( s.contains("max_tokens") && s["max_tokens"] == JSON::TYPE::INT )
                last.max_tokens = static_cast<size_t>(static_cast<long long>(s["max_tokens"]));
            if ( s.contains("tool_profile") && s["tool_profile"] == JSON::TYPE::STRING )
                last.tool_profile = s["tool_profile"].to_string();
            if ( s.contains("steering") && s["steering"] == JSON::TYPE::STRING )
                last.steering = s["steering"].to_string();
            if ( s.contains("steering_mode") && s["steering_mode"] == JSON::TYPE::STRING )
                last.steering_mode = s["steering_mode"].to_string();

            // Migrate a settings block written before context_auto/auto_compact
            // defaulted to on. Applied here, on the loaded state itself, so that
            // any later save_last_used() persists the migrated values rather than
            // re-writing the old ones with a current version stamp. A user who
            // set a real context limit kept it deliberately and is left alone.
            if ( last.settings_version < 1 ) {
                if ( !last.context_auto && last.context_limit == 0 ) {
                    last.context_auto = true;
                    last.auto_compact = true;
                    logger::notice["agent"]
                        << "enabling context auto-budget and auto-compaction (new defaults); "
                        << "turn them off with /settings context 0 and /settings auto_compact off"
                        << std::endl;
                }
                last.settings_version = Config::SETTINGS_VERSION;
            }
        }
    } catch ( const std::exception& e ) {
        logger::warning["config"] << "failed to parse state file: " << e.what() << std::endl;
    }
    return last;
}

// Serialise a full state (provider, per-provider models, and UI settings) to the
// state file atomically. Settings are only written once they have been recorded.
static void write_state(const std::string& home_dir, const Config::LastUsed& last) {
    JSON models = JSON::Object{};
    for ( const auto& [name, m] : last.models )
        models[name] = m;

    JSON j = JSON::Object{
        { "provider", last.provider },
        { "models", models }
    };
    if ( last.has_settings ) {
        j["settings"] = JSON::Object{
            { "settings_version", static_cast<long long>(Config::SETTINGS_VERSION) },
            { "theme", last.theme },
            { "thinking", last.thinking },
            { "multiline", last.multiline },
            { "thinking_stream", last.thinking_stream },
            { "thinking_collapse", last.thinking_collapse },
            { "context_auto", last.context_auto },
            { "context_limit", static_cast<long long>(last.context_limit) },
            { "auto_compact", last.auto_compact },
            { "auto_compact_max_tokens", static_cast<long long>(last.auto_compact_max_tokens) },
            { "confirm_tools", last.confirm_tools },
            { "insecure", last.insecure },
            { "workflow_autoresume", last.workflow_autoresume },
            { "bell", last.bell },
            { "advisor", last.advisor },
            { "advisor_model", last.advisor_model },
            { "paste_preview", static_cast<long long>(last.paste_preview) },
            { "tool_call_limit", static_cast<long long>(last.tool_call_limit) },
            { "max_tokens", static_cast<long long>(last.max_tokens) },
            { "tool_profile", last.tool_profile },
            { "steering", last.steering },
            { "steering_mode", last.steering_mode }
        };
    }

    std::string path = state_path(home_dir);
    std::string tmp = path + ".tmp";
    {
        std::ofstream ofd(tmp, std::ios::out | std::ios::trunc);
        if ( !ofd.is_open()) {
            logger::warning["config"] << "failed to write state file: " << tmp << std::endl;
            return;
        }
        ofd << j.dump_minified() << "\n";
        ofd.flush();
    }
    // Restrict to the owner before publishing: the state file is per-user and
    // there is no reason for it to be world-readable (matches the token file).
    chmod(tmp.c_str(), 0600);
    std::filesystem::rename(tmp, path);
}

void Config::save_last_used(const std::string& home_dir, const std::string& provider, const std::string& model) {
    // Merge into any existing state so other providers' models and the settings
    // block survive.
    LastUsed last = load_last_used(home_dir);
    last.provider = provider;
    if ( !model.empty())
        last.models[provider] = model;
    write_state(home_dir, last);
}

void Config::save_settings(const std::string& home_dir) const {
    // Preserve the existing provider/model block; overwrite the settings block.
    LastUsed last = load_last_used(home_dir);
    last.has_settings = true;
    last.theme = theme;
    last.thinking = thinking;
    last.multiline = multiline;
    last.thinking_stream = thinking_stream;
    last.thinking_collapse = thinking_collapse;
    last.context_auto = context_auto;
    last.context_limit = context_limit;
    last.auto_compact = auto_compact;
    last.auto_compact_max_tokens = auto_compact_max_tokens;
    last.workflow_autoresume = workflow_autoresume;
    // A CLI flag (-Y/-I/-T) sets the mode for this session only — it must NOT
    // overwrite the user's saved preference. `last` already holds the persisted
    // mode (loaded above), so keep it unless the mode is a deliberate in-session
    // choice (/tools clears tool_mode_explicit before saving).
    if ( !tool_mode_explicit ) {
        last.confirm_tools = confirm_tools;
        last.insecure = insecure;
    }
    last.bell = bell;
    last.advisor = advisor;
    last.advisor_model = advisor_model;
    last.paste_preview = paste_preview;
    last.tool_call_limit = tool_call_limit;
    last.max_tokens = max_tokens;
    last.tool_profile = tool_profile;
    last.steering = steering;
    last.steering_mode = steering_mode;
    write_state(home_dir, last);
}

void Config::apply_settings(const LastUsed& last) {
    if ( !last.has_settings )
        return;
    theme = last.theme;
    thinking = last.thinking;
    multiline = last.multiline;
    thinking_stream = last.thinking_stream;
    thinking_collapse = last.thinking_collapse;
    context_auto = last.context_auto;
    context_limit = last.context_limit;
    auto_compact = last.auto_compact;
    auto_compact_max_tokens = last.auto_compact_max_tokens;
    workflow_autoresume = last.workflow_autoresume;
    // Tool confirmation mode is persisted (user opted in), but an explicit CLI
    // flag (-T/-Y/-I) for this launch always wins over the saved mode.
    if ( !tool_mode_explicit ) {
        confirm_tools = last.confirm_tools;
        insecure = last.insecure;
    }
    if ( !last.tool_profile.empty() )
        tool_profile = last.tool_profile;
    if ( !last.steering.empty() )
        steering = last.steering;
    if ( !last.steering_mode.empty() )
        steering_mode = last.steering_mode;
    bell = last.bell;
    advisor = last.advisor;
    if ( !last.advisor_model.empty())
        advisor_model = last.advisor_model;
    paste_preview = last.paste_preview;
    // A config-file budget wins over the persisted one; otherwise restore what the
    // user last set in /settings.
    if ( !tool_call_limit_explicit )
        tool_call_limit = last.tool_call_limit;
    if ( !max_tokens_explicit )
        max_tokens = last.max_tokens;
}

void Config::ensure_home_dir() {
    if ( home_dir.empty())
        home_dir = default_home_dir();
    if ( !std::filesystem::exists(home_dir))
        std::filesystem::create_directories(home_dir);
}

} // namespace agent
