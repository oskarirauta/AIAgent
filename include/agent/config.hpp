#pragma once

#include <string>
#include <optional>
#include <map>
#include <vector>
#include "usage.hpp"
#include "json.hpp"

namespace agent {

// Per-model price in USD per one million tokens (input / output).
struct ModelPricing {
    double input_per_mtok = 0.0;
    double output_per_mtok = 0.0;
};

class Config {
public:
    std::string provider = "openai";
    std::string model = "gpt-4o-mini";
    std::string api_url = "https://api.openai.com/v1";
    std::string api_key;
    std::string oauth_host;
    std::string oauth_client_id;
    std::string log_level = "info";
    std::string system_prompt = "You are a helpful Linux CLI assistant.";
    std::string home_dir;
    std::string theme = "dark"; // colour theme: dark | light | warm
    bool multiline = false;     // multi-line prompt: show long input wrapped across lines
    std::string thinking;       // thinking/effort level (empty = provider default); applied by Kimi
    bool thinking_stream = true; // stream the model's reasoning live into the transcript
    bool thinking_collapse = false; // collapse mode: show reasoning live, then hide it once the answer is done
    bool tools_enabled = true;
    bool confirm_tools = true;  // ask before confirmation-requiring tools
    bool insecure = false;      // run every tool without asking (implies no danger warnings)
    bool strict = false;        // in confirm mode, ignore the safe-command allowlist
    size_t context_limit = 0;   // approx token budget for history sent to the model (0 = unlimited)
    bool context_auto = false;  // derive the budget from the model's known context window
    size_t max_tokens = 8192;   // cap on a single reply's output tokens (config: max_tokens)
    size_t tool_call_limit = 50; // per-turn tool-call cap before asking to continue (0 = unlimited)
    bool auto_compact = false;  // summarise history automatically when it nears the context budget
    size_t auto_compact_pct = 80; // trigger threshold as a percentage of context_budget()
    bool workflow_autoresume = false; // a finished workflow starts a turn by itself (bounded; see repl)
    bool supersede_tools = true; // elide stale tool results (older read/run of the same target)

    // Config-extensible command safety lists (config file only, never persisted
    // state and never project files): extra confirmation-free read-only commands,
    // and extra programs that always warn. Danger wins when a name is on both.
    std::vector<std::string> tools_safe;
    std::vector<std::string> tools_danger;
    bool advisor = false;       // expose a tool letting the model consult a stronger advisor model (claude only)
    std::string advisor_model = "claude-opus-4-8"; // the model consulted by the advisor tool

    // Cost / usage budget. Pricing is per-model (USD per million tokens), loaded
    // from `price.<model>: <in>/<out>` config keys — no built-in defaults, so a
    // model with no price shows usage only (matching flat-rate subscriptions).
    std::map<std::string, ModelPricing> pricing;
    double budget_usd = 0.0;    // warn when the session's estimated cost nears this (0 = off)
    size_t budget_tokens = 0;   // warn when the session's total tokens near this (0 = off)

    // web_search tool: lets the model look things up online (esp. useful for local
    // models with no MCP search server). Default on; disable for offline/private use.
    bool web_search = true;
    std::string web_search_url = "https://html.duckduckgo.com/html/";

    // Prompt caching: mark cache_control breakpoints (Anthropic/Claude) so the
    // stable prefix (tools + system + prior turns) is cached — cheaper + faster.
    // OpenAI/Kimi/DeepSeek cache automatically server-side, so this only changes
    // the Anthropic requests. On by default.
    bool prompt_cache = true;

    // Run independent read-only tool calls (read_file/grep/find_symbol/
    // list_directory) from one model turn concurrently. On by default.
    bool parallel_tools = true;

    // Path to an MCP servers config ({"mcpServers": {...}}). Empty = look in the
    // default locations (<home>/mcp.json and ./.mcp.json).
    std::string mcp_config;

    // ncurses paste detection thresholds
    size_t paste_threshold_chars = 500;        // characters for multi-line paste
    size_t paste_threshold_lines = 10;         // line breaks for multi-line paste
    size_t paste_single_line_chars = 350;      // characters for single-line paste
    size_t paste_threshold_ms      = 100;      // max milliseconds between pasted characters
    size_t paste_preview           = 8;        // echo only the first N lines of a paste (0 = all)

    // Provider-specific options, keyed by provider name. Loaded from config keys
    // like provider.<name>.<key>.
    std::map<std::string, JSON> provider_options;

    // Whether provider / model were set explicitly (CLI flag or config file), as
    // opposed to left at their built-in defaults. Used to decide when the
    // last-used state may fill them in.
    bool provider_explicit = false;
    bool model_explicit = false;

    void load(const std::string& path);
    void apply_cli(const usage_t& usage);
    void ensure_home_dir();

    // Last-used provider and per-provider model, persisted so a bare launch
    // resumes the previous session's provider/model.
    struct LastUsed {
        std::string provider;
        std::map<std::string, std::string> models;
        std::string model_for(const std::string& p) const {
            auto it = models.find(p);
            return it == models.end() ? std::string() : it->second;
        }
        // Persisted UI/behaviour settings (only meaningful when has_settings).
        bool has_settings = false;
        std::string theme;
        std::string thinking;
        bool multiline = false;
        bool thinking_stream = true;
        bool thinking_collapse = false;
        size_t context_limit = 0;
        bool context_auto = false;
        bool auto_compact = false;
        bool workflow_autoresume = false;
        bool advisor = false;
        std::string advisor_model;
        size_t paste_preview = 8;
    };
    static LastUsed load_last_used(const std::string& home_dir);
    static void save_last_used(const std::string& home_dir, const std::string& provider, const std::string& model);

    // Persist the UI/behaviour settings (theme, multiline, thinking, context) of
    // this config to the state file, preserving the last provider/model. Security
    // settings (tool mode, strict) are intentionally NOT persisted.
    void save_settings(const std::string& home_dir) const;

    // Apply persisted settings from a loaded state onto this config.
    void apply_settings(const LastUsed& last);

    static std::string default_path();
    static std::string default_home_dir();

    // Expand a leading "~" or "~/" to $HOME (falling back to /root).
    static std::string expand_tilde(const std::string& path);

    // Parse an unsigned size with an optional K/M/G suffix (K = 1024), e.g.
    // "64K" -> 65536. Returns `fallback` (and does not throw) on malformed input.
    static size_t parse_size_suffixed(const std::string& value, size_t fallback);

    // Provider-appropriate default model, used when the user did not pass -m and
    // left `model` at its built-in default.
    static std::string default_model_for(const std::string& provider);

    // Provider-appropriate default system prompt (identity), used when the user
    // did not override `system_prompt`.
    static std::string default_system_prompt_for(const std::string& provider);

    // Approximate context window (tokens) known for a model, or 0 if unknown.
    static size_t context_window_for(const std::string& model);

    // Price for `model`: exact match first, then the first pricing entry whose key
    // is a substring of the model (so "gpt-4o" covers "gpt-4o-2024-..."). Empty if
    // no price is configured (e.g. a flat-rate subscription).
    std::optional<ModelPricing> pricing_for(const std::string& model) const;

    // Estimated session cost in USD for the current model, or -1 if unpriced.
    double session_cost(long input_tokens, long output_tokens, long cached_input = 0) const;

    // The token budget to actually apply when trimming history: the model's
    // window (with response headroom) in auto mode, else `context_limit`.
    // 0 means no limit.
    size_t context_budget() const;
};

} // namespace agent
