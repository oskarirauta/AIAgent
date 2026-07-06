#pragma once

#include <string>
#include <optional>
#include <map>
#include "usage.hpp"
#include "json.hpp"

namespace agent {

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
    bool tools_enabled = true;
    bool confirm_tools = true;  // ask before confirmation-requiring tools
    bool insecure = false;      // run every tool without asking (implies no danger warnings)
    bool strict = false;        // in confirm mode, ignore the safe-command allowlist

    // ncurses paste detection thresholds
    size_t paste_threshold_chars = 500;        // characters for multi-line paste
    size_t paste_threshold_lines = 10;         // line breaks for multi-line paste
    size_t paste_single_line_chars = 350;      // characters for single-line paste
    size_t paste_threshold_ms      = 100;      // max milliseconds between pasted characters

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
    };
    static LastUsed load_last_used(const std::string& home_dir);
    static void save_last_used(const std::string& home_dir, const std::string& provider, const std::string& model);

    static std::string default_path();
    static std::string default_home_dir();

    // Expand a leading "~" or "~/" to $HOME (falling back to /root).
    static std::string expand_tilde(const std::string& path);

    // Provider-appropriate default model, used when the user did not pass -m and
    // left `model` at its built-in default.
    static std::string default_model_for(const std::string& provider);

    // Provider-appropriate default system prompt (identity), used when the user
    // did not override `system_prompt`.
    static std::string default_system_prompt_for(const std::string& provider);
};

} // namespace agent
