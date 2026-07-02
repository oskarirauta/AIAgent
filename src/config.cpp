#include "config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "common.hpp"
#include "lowercase_map.hpp"
#include "logger.hpp"

namespace agent {

std::string Config::default_path() {
    const char* home = std::getenv("HOME");
    if ( !home || !*home )
        home = "/root";
    return std::string(home) + "/.config/ai-agent/config";
}

void Config::load(const std::string& path) {

    if ( path.empty())
        return;

    if ( !std::filesystem::exists(path)) {
        logger::verbose["config"] << "config file not found: " << path << std::endl;
        return;
    }

    logger::verbose["config"] << "loading config from " << path << std::endl;

    try {
        common::lowercase_map<std::string> cfg = common::parseFile(path, ':');

        if ( cfg.contains("provider"))
            provider = common::trimmed(cfg["provider"], common::whitespace);
        if ( cfg.contains("model"))
            model = common::trimmed(cfg["model"], common::whitespace);
        if ( cfg.contains("api_url"))
            api_url = common::trimmed(cfg["api_url"], common::whitespace);
        if ( cfg.contains("api_key"))
            api_key = common::trimmed(cfg["api_key"], common::whitespace);
        if ( cfg.contains("log_level"))
            log_level = common::trimmed(cfg["log_level"], common::whitespace);
        if ( cfg.contains("system_prompt"))
            system_prompt = common::trimmed(cfg["system_prompt"], common::whitespace);

    } catch ( const std::exception& e ) {
        logger::warning["config"] << "failed to read config: " << e.what() << std::endl;
    }
}

void Config::apply_cli(const usage_t& usage) {

    if ( usage["provider"] )
        provider = usage["provider"].stringValue();
    if ( usage["model"] )
        model = usage["model"].stringValue();
    if ( usage["api_url"] )
        api_url = usage["api_url"].stringValue();
    if ( usage["api_key"] )
        api_key = usage["api_key"].stringValue();
    if ( usage["log_level"] )
        log_level = usage["log_level"].stringValue();
    if ( usage["system_prompt"] )
        system_prompt = usage["system_prompt"].stringValue();
}

} // namespace agent
