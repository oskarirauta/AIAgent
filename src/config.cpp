#include "agent/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include "common.hpp"
#include "logger.hpp"

namespace agent {

static std::string trim(const std::string& s) {
    return common::trim_ws(s);
}

// One place for boolean-ish config values so every flag accepts the same set
// (true/1/yes/on) — previously some keys silently rejected "on".
static bool parse_bool(const std::string& value) {
    std::string v = common::to_lower(common::trim_ws(value));
    return v == "true" || v == "1" || v == "yes" || v == "on";
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
    const char* home = std::getenv("HOME");
    if ( !home || !*home )
        home = "/root";
    return std::string(home) + "/.config/ai-agent/config";
}

std::string Config::default_model_for(const std::string& provider) {
    if ( provider == "claude" ) return "claude-opus-4-8";
    if ( provider == "anthropic" ) return "claude-opus-4-8";
    if ( provider == "kimi" ) return "kimi-for-coding";     // managed:kimi-code / "K2.7 Code"
    if ( provider == "moonshot" ) return "kimi-k2-0905-preview";
    if ( provider == "openrouter" ) return "openrouter/free"; // auto-routes to an available free model; -m for a specific one
    if ( provider == "ollama" ) return "llama3";
    return "gpt-4o-mini"; // openai and any other OpenAI-compatible provider
}

std::string Config::default_system_prompt_for(const std::string& provider) {
    if ( provider == "kimi" )
        return "You are Kimi, an AI assistant built by Moonshot AI, running as a "
               "command-line coding assistant. You help with software engineering "
               "tasks on a Linux system. Be concise and precise.";
    if ( provider == "claude" )
        // The Claude provider additionally injects the required Claude Code
        // identity block, so this second block only sets task context.
        return "You are Claude Code, Anthropic's official CLI, assisting with "
               "software engineering tasks on a Linux system. Be concise and precise.";
    return "You are a helpful Linux CLI assistant.";
}

size_t Config::context_window_for(const std::string& model) {
    std::string m = common::to_lower(model);
    auto has = [&](const char* p) { return m.find(p) != std::string::npos; };
    // Explicit 1M-context variants first.
    if ( has("1m") ) return 1000000;
    if ( has("claude") || has("opus") || has("sonnet") || has("haiku") || has("fable") )
        return 200000;
    if ( has("kimi") || has("moonshot") ) return 256000;
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

double Config::session_cost(long input_tokens, long output_tokens) const {
    auto p = pricing_for(model);
    if ( !p )
        return -1.0;
    return static_cast<double>(input_tokens) / 1e6 * p->input_per_mtok +
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
        else if ( key == "multiline" ) multiline = parse_bool(value);
        else if ( key == "thinking_stream" ) thinking_stream = parse_bool(value);
        else if ( key == "thinking_collapse" ) thinking_collapse = parse_bool(value);
        else if ( key == "system_prompt" ) system_prompt = value;
        else if ( key == "home_dir" ) home_dir = expand_tilde(value);
        else if ( key == "tools_enabled" ) tools_enabled = parse_bool(value);
        else if ( key == "strict" ) strict = parse_bool(value);
        else if ( key == "context_limit" ) {
            if ( common::to_lower(value) == "auto" ) { context_auto = true; }
            else { context_auto = false; context_limit = parse_size(value, context_limit, key); }
        }
        else if ( key == "auto_compact" ) auto_compact = parse_bool(value);
        else if ( key == "auto_compact_pct" ) auto_compact_pct = parse_size(value, auto_compact_pct, key);
        else if ( key == "workflow_autoresume" ) workflow_autoresume = parse_bool(value);
        else if ( key == "advisor" ) advisor = parse_bool(value);
        else if ( key == "advisor_model" ) advisor_model = value;
        else if ( key == "budget_tokens" ) budget_tokens = parse_size(value, budget_tokens, key);
        else if ( key == "web_search" ) web_search = parse_bool(value);
        else if ( key == "web_search_url" ) web_search_url = value;
        else if ( key == "prompt_cache" ) prompt_cache = parse_bool(value);
        else if ( key == "parallel_tools" ) parallel_tools = parse_bool(value);
        else if ( key == "mcp_config" ) mcp_config = expand_tilde(value);
        else if ( key == "budget_usd" ) {
            try { budget_usd = std::stod(common::trim_ws(value)); }
            catch ( ... ) { logger::warning["config"] << "invalid budget_usd: " << value << std::endl; }
        }
        else if ( key.rfind("price.", 0) == 0 ) {
            // price.<model>: <input>/<output>  (USD per million tokens)
            std::string model_key = trim(key.substr(6));
            size_t slash = value.find('/');
            if ( model_key.empty() || slash == std::string::npos ) {
                logger::warning["config"] << "invalid price entry: " << key << ": " << value << std::endl;
            } else {
                try {
                    ModelPricing p;
                    p.input_per_mtok = std::stod(trim(value.substr(0, slash)));
                    p.output_per_mtok = std::stod(trim(value.substr(slash + 1)));
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
            if ( s.contains("workflow_autoresume") && s["workflow_autoresume"] == JSON::TYPE::BOOL )
                last.workflow_autoresume = s["workflow_autoresume"].to_bool();
            if ( s.contains("advisor") && s["advisor"] == JSON::TYPE::BOOL )
                last.advisor = s["advisor"].to_bool();
            if ( s.contains("advisor_model") && s["advisor_model"] == JSON::TYPE::STRING )
                last.advisor_model = s["advisor_model"].to_string();
            if ( s.contains("paste_preview") && s["paste_preview"] == JSON::TYPE::INT )
                last.paste_preview = static_cast<size_t>(static_cast<long long>(s["paste_preview"]));
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
            { "theme", last.theme },
            { "thinking", last.thinking },
            { "multiline", last.multiline },
            { "thinking_stream", last.thinking_stream },
            { "thinking_collapse", last.thinking_collapse },
            { "context_auto", last.context_auto },
            { "context_limit", static_cast<long long>(last.context_limit) },
            { "auto_compact", last.auto_compact },
            { "workflow_autoresume", last.workflow_autoresume },
            { "advisor", last.advisor },
            { "advisor_model", last.advisor_model },
            { "paste_preview", static_cast<long long>(last.paste_preview) }
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
    last.workflow_autoresume = workflow_autoresume;
    last.advisor = advisor;
    last.advisor_model = advisor_model;
    last.paste_preview = paste_preview;
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
    workflow_autoresume = last.workflow_autoresume;
    advisor = last.advisor;
    if ( !last.advisor_model.empty())
        advisor_model = last.advisor_model;
    paste_preview = last.paste_preview;
}

void Config::ensure_home_dir() {
    if ( home_dir.empty())
        home_dir = default_home_dir();
    if ( !std::filesystem::exists(home_dir))
        std::filesystem::create_directories(home_dir);
}

} // namespace agent
