#include <iostream>
#include <filesystem>
#include <cstdlib>

#include "usage.hpp"
#include "logger.hpp"
#include "agent/config.hpp"
#include "agent/repl.hpp"
#include "agent/signal_handler.hpp"

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
        { "AI Agent", "\nversion ", "0.1.0", "author ", "Oskari Rauta", "copyright ", "2026, Oskari Rauta", "\nusage:\n   ", "[options] [prompt]", "\nOptions:", "\nUniversal Linux CLI AI Agent\n" },
        {
            { "help", { "h", "help", "show usage help" }},
            { "version", { "v", "version", "show version" }},
            { "config", { "c", "config", "path to config file", usage_t::OPTIONAL }},
            { "provider", { "p", "provider", "ai provider: openai or ollama", usage_t::OPTIONAL }},
            { "model", { "m", "model", "model name", usage_t::OPTIONAL }},
            { "api_url", { "u", "api-url", "api endpoint url", usage_t::OPTIONAL }},
            { "api_key", { "k", "api-key", "api key / token", usage_t::OPTIONAL }},
            { "log_level", { "l", "log-level", "quiet/error/warning/notice/info/verbose/vverbose/debug", usage_t::OPTIONAL }},
            { "system_prompt", { "s", "system-prompt", "system prompt message", usage_t::OPTIONAL }},
            { "home_dir", { "d", "home", "agent home directory for memory and data", usage_t::OPTIONAL }},
            { "prompt", { "P", "prompt", "single prompt mode, exit after answer", usage_t::OPTIONAL }}
        }
    };
}

int main(int argc, char **argv) {

    usage_t usage = make_usage(argc, argv);

    if ( !usage.validated ) {
        std::cerr << usage.title() << "\n" << usage.errors() << std::endl;
        return 1;
    }

    if ( usage["help"] ) {
        std::cout << usage << "\n" << usage.help() << std::endl;
        return 0;
    }

    if ( usage["version"] ) {
        std::cout << usage.version() << std::endl;
        return 0;
    }

    agent::Config config;

    std::string config_path = agent::Config::default_path();
    if ( usage["config"] )
        config_path = usage["config"].stringValue();

    config.load(config_path);
    config.apply_cli(usage);
    config.ensure_home_dir();

    agent::install_signal_handlers();

    set_log_level(config.log_level);

    if ( config.provider != "openai" && config.provider != "ollama" &&
         config.provider != "anthropic" && config.provider != "moonshot" ) {
        logger::error << "unsupported provider: " << config.provider << ". use openai, ollama, anthropic or moonshot." << std::endl;
        return 1;
    }

    if ( config.api_key.empty() && config.provider != "ollama" ) {
        logger::warning << "no api-key configured for " << config.provider << " provider" << std::endl;
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

    logger::info["agent"] << "provider: " << config.provider << ", model: " << config.model << std::endl;
    logger::info["agent"] << "home dir: " << config.home_dir << std::endl;

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

    return 0;
}
