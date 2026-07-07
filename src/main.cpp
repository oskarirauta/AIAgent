#include <iostream>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>
#include <curl/curl.h>

#include "usage.hpp"
#include "logger.hpp"
#include "env.hpp"
#include "agent/config.hpp"
#include "agent/commands.hpp"
#include "agent/repl.hpp"
#include "agent/signal_handler.hpp"
#include "agent/api/client.hpp"
#include "agent/providers/provider.hpp"

static void set_log_level(const std::string& s) {
    std::string lvl = common::to_lower(s);
    if ( lvl == "quiet" ) logger::silence = true;
    else if ( lvl == "error" ) logger::loglevel(logger::error);
    else if ( lvl == "warning" ) logger::loglevel(logger::warning);
    else if ( lvl == "notice" ) logger::loglevel(logger::notice);
    else if ( lvl == "info" ) logger::loglevel(logger::info);
    else if ( lvl == "verbose" ) logger::loglevel(logger::verbose);
    else if ( lvl == "vverbose" ) logger::loglevel(logger::vverbose);
    else if ( lvl == "debug" ) logger::loglevel(logger::debug);
    else logger::loglevel(logger::info);
}

static usage_t make_usage(int argc, char **argv) {

    return usage_t{
        { argc, argv },
        { "AI Agent", "\nversion ", agent::VERSION, "author ", "Oskari Rauta", "copyright ", "2026, Oskari Rauta", "\nusage:\n   ", "[options] [prompt]", "\nOptions:",
          "\nUniversal Linux CLI AI assistant — a provider-agnostic alternative to Kimi Code / Claude Code.\n"
          "With no prompt it starts an interactive REPL; type /help there for the in-app commands.\n"
          "Config: ~/.config/ai-agent/config (or -c). Data/credentials/memory: ~/.local/share/ai-agent (or -d).\n" },
        {
            { "help", { "h", "help", "show usage help" }},
            { "version", { "v", "version", "show version" }},
            { "config", { "c", "config", "path to config file", usage_t::OPTIONAL }},
            { "provider", { "p", "provider", "ai provider: openai, ollama, anthropic, moonshot, openrouter, kimi or claude", usage_t::OPTIONAL }},
            { "model", { "m", "model", "model name", usage_t::OPTIONAL }},
            { "api_url", { "u", "api-url", "api endpoint url", usage_t::OPTIONAL }},
            { "api_key", { "k", "api-key", "api key / token", usage_t::OPTIONAL }},
            { "login", { "L", "login", "force re-authentication for the selected provider and exit" }},
            { "log_level", { "l", "log-level", "quiet/error/warning/notice/info/verbose/vverbose/debug", usage_t::OPTIONAL }},
            { "system_prompt", { "s", "system-prompt", "system prompt message", usage_t::OPTIONAL }},
            { "home_dir", { "d", "home", "agent home directory for memory and data", usage_t::OPTIONAL }},
            { "no_tools", { "T", "no-tools", "disable tool calls (safer mode)" }},
            { "yes_tools", { "Y", "yes-tools", "run ordinary tools without confirmation (danger-listed commands still warn)" }},
            { "insecure", { "I", "insecure", "run ALL tools without any confirmation, including dangerous commands" }},
            { "prompt", { "P", "prompt", "single prompt mode, exit after answer", usage_t::OPTIONAL }},
            { "output_format", { "o", "output-format", "single-prompt output: text (default) or json", usage_t::OPTIONAL }},
            { "dump_commands", { "", "dump-commands", "print the slash-command reference as Markdown (regenerates COMMANDS.md) and exit" }}
        }
    };
}

int main(int argc, char **argv) {

    // Own libcurl's global state explicitly instead of letting it initialise
    // lazily. Declared first so its cleanup runs last — after every Client (and
    // its easy handle) has been destroyed.
    struct CurlGlobal {
        CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
        ~CurlGlobal() { curl_global_cleanup(); }
    } curl_global;

    usage_t usage = make_usage(argc, argv);

    if ( !usage.validated ) {
        std::cerr << usage.title() << "\n" << usage.errors() << "\n" << std::endl;
        return 1;
    }

    if ( usage["help"] ) {
        std::cout << usage << "\n" << usage.help() << "\n" << std::endl;
        return 0;
    }

    if ( usage["version"] ) {
        std::cout << usage.version() << std::endl;
        return 0;
    }

    if ( usage["dump_commands"] ) {
        std::cout << agent::commands_markdown();
        return 0;
    }

    agent::Config config;

    std::string config_path = agent::Config::default_path();
    if ( usage["config"] )
        config_path = agent::Config::expand_tilde(usage["config"].stringValue());

    config.load(config_path);
    config.apply_cli(usage);
    config.ensure_home_dir();

    agent::install_signal_handlers();

    set_log_level(config.log_level);

    // Send the full log to a file so it never has to clutter the conversation.
    // (The interactive REPL additionally limits the terminal to errors only.)
    static std::ofstream log_file;
    {
        std::string log_dir = config.home_dir + "/logs";
        std::filesystem::create_directories(log_dir);
        log_file.open(log_dir + "/agent.log", std::ios::app);
        if ( log_file.is_open())
            logger::file_stream = &log_file;
    }

    // In the interactive UI, keep the terminal quiet (errors only) so logging
    // never lands in the transcript — everything still goes to the log file.
    if ( isatty(STDIN_FILENO))
        logger::loglevel(logger::error);

    // Resume the last-used provider/model when the user did not specify them.
    // Provider is resolved first; the model then follows the resolved provider
    // (its own remembered model, else a provider-appropriate default).
    agent::Config::LastUsed last_used = agent::Config::load_last_used(config.home_dir);
    if ( !config.provider_explicit && !last_used.provider.empty()) {
        config.provider = last_used.provider;
    }
    // Restore persisted UI/behaviour settings (theme, multiline, thinking,
    // context) from the previous session. Tool mode / strict are never persisted.
    config.apply_settings(last_used);

    if ( config.provider != "openai" && config.provider != "ollama" &&
         config.provider != "anthropic" && config.provider != "moonshot" &&
         config.provider != "openrouter" &&
         config.provider != "kimi" && config.provider != "claude" ) {
        logger::error << "unsupported provider: " << config.provider << ". use openai, ollama, anthropic, moonshot, openrouter, kimi or claude." << std::endl;
        return 1;
    }

    if ( config.provider == "kimi" && config.api_url == agent::Config().api_url ) {
        config.api_url = "https://api.kimi.com/coding/v1";
    }

    // Resolve the model once the provider is final: keep an explicit -m/config
    // model, otherwise reuse this provider's remembered model, otherwise fall
    // back to a provider-appropriate default.
    if ( !config.model_explicit ) {
        std::string remembered = last_used.model_for(config.provider);
        config.model = !remembered.empty() ? remembered
                                           : agent::Config::default_model_for(config.provider);
    }

    // Persist the resolved provider/model as the new last-used state.
    agent::Config::save_last_used(config.home_dir, config.provider, config.model);

    // Give each provider a fitting identity when the user did not set a custom
    // system prompt, so e.g. Kimi knows it is Kimi.
    if ( config.system_prompt == agent::Config().system_prompt ) {
        config.system_prompt = agent::Config::default_system_prompt_for(config.provider);
    }

    // API-key providers: if no key was given (-k / config), fall back to the
    // conventional environment variables, then a generic one. (Kimi/Claude use
    // OAuth and Ollama needs none.)
    if ( config.api_key.empty() && config.provider != "ollama" &&
         config.provider != "kimi" && config.provider != "claude" ) {
        const char* provider_var =
            config.provider == "openrouter" ? "OPENROUTER_API_KEY" :
            config.provider == "moonshot"  ? "MOONSHOT_API_KEY" :
            config.provider == "anthropic" ? "ANTHROPIC_API_KEY" :
            config.provider == "openai"    ? "OPENAI_API_KEY" : nullptr;
        if ( provider_var && env[provider_var] )
            config.api_key = env[provider_var].str();
        else if ( env["AI_AGENT_API_KEY"] )
            config.api_key = env["AI_AGENT_API_KEY"].str();

        if ( !config.api_key.empty())
            logger::info["agent"] << "using api key from " << ( provider_var ? provider_var : "AI_AGENT_API_KEY" ) << std::endl;
    }

    if ( config.api_key.empty() && config.provider != "ollama" &&
         config.provider != "kimi" && config.provider != "claude" ) {
        std::string var =
            config.provider == "openrouter" ? "OPENROUTER_API_KEY" :
            config.provider == "moonshot"  ? "MOONSHOT_API_KEY" :
            config.provider == "anthropic" ? "ANTHROPIC_API_KEY" :
            config.provider == "openai"    ? "OPENAI_API_KEY" : "AI_AGENT_API_KEY";
        logger::warning << "no api-key for " << config.provider
                        << " — pass -k <key>, set `api_key:` in the config, or export "
                        << var << std::endl;
    }

    std::string prompt;
    if ( usage["prompt"] ) {
        prompt = usage["prompt"].stringValue();
    } else {
        auto remainder = usage.remainder();
        if ( !remainder.empty()) {
            prompt = common::join_vector(remainder, " ");
        }
    }
    if ( usage["output_format"] ) {
        std::string of = usage["output_format"].stringValue();
        config.output_format = ( of == "json" ) ? "json" : "text";
    }

    logger::info["agent"] << "provider: " << config.provider << ", model: " << config.model << std::endl;
    logger::info["agent"] << "home dir: " << config.home_dir << std::endl;

    if ( config.provider == "kimi" || config.provider == "claude" ) {
        agent::api::Client client;
        auto provider = agent::providers::create(config);
        if ( !provider->authenticate(client, usage["login"]) ) {
            logger::error << config.provider << " authentication failed" << "\n" << std::endl;
            return 1;
        }
        if ( usage["login"] ) {
            logger::info[config.provider] << "login successful, token saved" << "\n" << std::endl;
            return 0;
        }
    }

    try {
        agent::Repl repl(config);
        if ( prompt.empty())
            repl.run();
        else
            repl.run_once(prompt);
    } catch ( const std::exception& e ) {
        logger::error << e.what() << std::endl;
        return 1;
    }

    std::cout << "\n\n" << std::endl;

    return 0;
}
