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
    double cache_read_ratio = 0.0;  // 0.0 means default to provider pricing rules
    double cache_write_ratio = 0.0; // 0.0 means default to provider pricing rules
};

class Config {
public:
    // Schema version of the persisted settings block (state.json). Bumped when a
    // default changes in a way that an existing state must be migrated to.
    static constexpr int SETTINGS_VERSION = 1;

    std::string provider = "openai";
    std::string model = "gpt-4o-mini";
    std::string api_url = "https://api.openai.com/v1";
    std::string api_key;
    std::string oauth_host;
    std::string oauth_client_id;
    std::string log_level = "info";
    std::string system_prompt = "You are a helpful Linux CLI assistant.";
    std::string home_dir;
    std::string output_format = "text"; // single-prompt (-P) output: text | json
    std::string theme = "dark"; // colour theme: dark | light | warm | cool | rose | custom
    // The "custom" theme: a base palette plus per-role colour overrides, written
    // in the config file as `theme_base: cool` and `theme.ai: #7aa2f7` /
    // `theme.dim: 244`. Config-file only (they are a palette, not a toggle).
    std::string theme_base = "dark";
    std::map<std::string, std::string> theme_colors; // role -> colour spec
    bool multiline = false;     // multi-line prompt: show long input wrapped across lines
    std::string thinking;       // thinking/effort level (empty = provider default); applied by Kimi
    bool thinking_stream = true; // stream the model's reasoning live into the transcript
    bool thinking_collapse = false; // collapse mode: show reasoning live, then hide it once the answer is done
    bool tools_enabled = true;
    bool confirm_tools = true;  // ask before confirmation-requiring tools
    bool insecure = false;      // run every tool without asking (implies no danger warnings)
    bool tool_mode_explicit = false; // a CLI flag (-T/-Y/-I) set the mode; don't let saved state override it
    bool strict = false;        // in confirm mode, ignore the safe-command allowlist
    bool plan_mode = false;     // read-only planning: mutating tools are blocked (session-only)
    std::string tool_profile = "code"; // active tool profile: full|code|research|review|minimal (default: code)
    std::string steering;       // persistent steering guidance for the model (empty = none)
    std::string steering_mode = "checkpoint"; // steering behavior: checkpoint|next_turn
    bool steal_lock = false;    // --steal-lock: take over a session locked by a live agent (session-only)
    // Named session within this project: several conversations can live side by
    // side in one directory (e.g. one building, one reviewing), each with its own
    // history file and lock. Empty = the project's default session.
    std::string session_name;
    size_t context_limit = 0;   // approx token budget for history sent to the model (0 = unlimited)
    bool context_auto = true;   // derive the budget from the model's known context window
    // Cap on a single reply's output tokens (config: max_tokens). Providers clamp
    // this to the model's own ceiling, so a high value means "as much as this
    // model allows" rather than a hard request for that many tokens.
    size_t max_tokens = 64000;
    // Per-turn tool-call cap before asking whether to continue (0 = unlimited).
    // Sized for real work: a first pass over a new project, or a refactor across
    // many files, routinely runs well past a hundred calls, and the prompt is a
    // runaway-loop guard rather than a budget the user should meet routinely.
    size_t tool_call_limit = 100;
    bool auto_compact = true;   // summarise history automatically when it nears the context budget
    size_t auto_compact_pct = 80; // trigger threshold as a percentage of context_budget()
    size_t auto_compact_max_tokens = 30000; // upper ceiling for auto-compact trigger (prevents 1M-window models from running up massive context before compacting)
    bool workflow_autoresume = false; // a finished workflow starts a turn by itself (bounded; see repl)
    std::string bell = "attention"; // terminal bell policy: never|question|attention|always
    bool supersede_tools = true; // elide stale tool results (older read/run of the same target)
    bool redact_secrets = true; // mask credentials in tool output before sending to the provider

    // Config-extensible command safety lists (config file only, never persisted
    // state and never project files): extra confirmation-free read-only commands,
    // and extra programs that always warn. Danger wins when a name is on both.
    std::vector<std::string> tools_safe;
    std::vector<std::string> tools_danger;

    // Ordered fallback providers: if a request fails hard (persistent 429/5xx or a
    // network error) before anything streamed, retry the turn on the next one that
    // is configured and logged in. Config file only. e.g. `failover: kimi,openai`.
    std::vector<std::string> failover;

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

    // Paste detection thresholds (bracketed paste + fast-input heuristics)
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
    // Budgets set explicitly in the config file are authoritative: they must not
    // be overwritten by the value persisted from a previous session.
    bool max_tokens_explicit = false;
    bool tool_call_limit_explicit = false;

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
        size_t auto_compact_max_tokens = 50000;
        // Schema version of the persisted settings block. Absent/0 means a state
        // written before context_auto and auto_compact defaulted to on; such a
        // state is migrated once so an existing user is not left with an
        // untrimmed context that eventually 400s mid-session.
        int settings_version = 0;
        bool workflow_autoresume = false;
        bool confirm_tools = true;  // persisted tool mode (confirm/auto/insecure)
        bool insecure = false;
        std::string bell = "attention";
        bool advisor = false;
        std::string advisor_model;
        size_t paste_preview = 8;
        size_t tool_call_limit = 100;
        size_t max_tokens = 64000;
        std::string tool_profile = "code";
        std::string steering;
        std::string steering_mode = "checkpoint";
    };
    static LastUsed load_last_used(const std::string& home_dir);
    static void save_last_used(const std::string& home_dir, const std::string& provider, const std::string& model);

    // Persist the UI/behaviour settings (theme, multiline, thinking, context) of
    // this config to the state file, preserving the last provider/model. The tool
    // confirmation mode is persisted when the user chose it (via /tools or /settings),
    // but a CLI flag (-T/-Y/-I, i.e. tool_mode_explicit) is session-only and never
    // written back over the saved preference; `strict` is not persisted.
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

    // Token counts use DECIMAL K/M ("32K" = 32000, "1.5M" = 1500000), unlike the
    // 1024-based helper above: a user asking for "100K tokens" means 100000.
    // format_tokens() prints what parse_tokens() accepts, so a value shown in the
    // UI can be typed straight back in.
    static std::string format_tokens(size_t n);
    static size_t parse_tokens(const std::string& value, size_t fallback);
    // Tool budgets share the same numeric syntax as token counts, but also
    // accept "unlimited"/"all" to mean 0.
    static size_t parse_tool_limit(const std::string& value, size_t fallback);

    // Provider-appropriate default model, used when the user did not pass -m and
    // left `model` at its built-in default.
    static std::string default_model_for(const std::string& provider);

    // Model annotations can carry opt-in capability hints without changing the
    // actual API model name. Example: `claude-opus-4-8[1m]` means "send the
    // base model name, but treat it as a 1M-context variant".
    static std::string base_model_name(const std::string& model);
    static bool model_requests_1m_context(const std::string& model);

    // The curated shortlist of well-known models for a provider, best first.
    // Used by the /model picker (when the provider has no live listing) and as
    // the candidate set for resolve_model(). Empty for providers whose models are
    // user-supplied (ollama) or an open namespace (openrouter, openai-compatible).
    static const std::vector<std::string>& known_models_for(const std::string& provider);

    // Outcome of resolving a user-typed model name.
    struct ModelMatch {
        std::string model;              // the name to actually use
        bool corrected = false;         // the input was rewritten to `model`
        std::vector<std::string> alternatives; // other plausible candidates (for a hint)
    };

    // Map a loosely-typed model name onto a known one: "fable" -> "claude-fable-5",
    // "sonet" -> "claude-sonnet-4-6". An exact known name is never touched, and an
    // input with no plausible candidate is returned unchanged (so any model the
    // provider offers but we do not list still works). `candidates` may carry a
    // provider's live model listing; the curated list is used when it is empty.
    static ModelMatch resolve_model(const std::string& provider, const std::string& input,
                                    const std::vector<std::string>& candidates = {});

    // Provider-appropriate default system prompt (identity), used when the user
    // did not override `system_prompt`.
    static std::string default_system_prompt_for(const std::string& provider);

    // Approximate context window (tokens) known for a model, or 0 if unknown.
    static size_t context_window_for(const std::string& model);

    // Price for `model`: exact match first, then the first pricing entry whose key
    // is a substring of the model (so "gpt-4o" covers "gpt-4o-2024-..."). Empty if
    // no price is configured (e.g. a flat-rate subscription).
    std::optional<ModelPricing> pricing_for(const std::string& model) const;

    struct ProviderPricingRules {
        double cache_read_ratio = 0.10;
        double cache_write_ratio = 1.00;
        int discount_pct() const { return static_cast<int>((1.0 - cache_read_ratio) * 100.0 + 0.5); }
    };

    ProviderPricingRules provider_pricing_rules(const std::string& prov, const std::string& mdl) const;

    // A session name reduced to a filename-safe token: letters, digits, '-' and
    // '_' survive, anything else becomes '-'. "default" (and an empty name) mean
    // the project's default session and normalise to "".
    static std::string sanitize_session_name(const std::string& name);

    // Estimated session cost in USD for the current model, or -1 if unpriced.
    double session_cost(long input_tokens, long output_tokens, long cached_input = 0, long cache_creation = 0) const;

    // The token budget to actually apply when trimming history: the model's
    // window (with response headroom) in auto mode, else `context_limit`.
    // 0 means no limit.
    size_t context_budget() const;

    // The budget auto-compaction measures against. Same as context_budget(), but
    // falls back to the model's own window when the context is "unlimited" — so
    // auto-compact still protects a long session instead of quietly never firing.
    // 0 only when the model's window is unknown.
    size_t compaction_budget() const;
};

} // namespace agent
