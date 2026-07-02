#include "agent/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "common.hpp"
#include "logger.hpp"

namespace agent {

static std::string trim(const std::string& s) {
    return common::trim_ws(s);
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

std::string Config::default_path() {
    const char* home = std::getenv("HOME");
    if ( !home || !*home )
        home = "/root";
    return std::string(home) + "/.config/ai-agent/config";
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

        if ( key == "provider" ) provider = value;
        else if ( key == "model" ) model = value;
        else if ( key == "api_url" ) api_url = value;
        else if ( key == "api_key" ) api_key = value;
        else if ( key == "log_level" ) log_level = value;
        else if ( key == "system_prompt" ) system_prompt = value;
        else if ( key == "home_dir" ) home_dir = value;
        else if ( key == "tools_enabled" ) tools_enabled = (common::to_lower(value) == "true" || value == "1" || common::to_lower(value) == "yes");
        else if ( key == "confirm_tools" ) confirm_tools = (common::to_lower(value) == "true" || value == "1" || common::to_lower(value) == "yes");
    }

    if ( home_dir.empty())
        home_dir = default_home_dir();
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
    if ( usage["home_dir"] )
        home_dir = usage["home_dir"].stringValue();
    if ( usage["no_tools"] )
        tools_enabled = false;
    if ( usage["yes_tools"] )
        confirm_tools = false;
}

void Config::ensure_home_dir() {
    if ( home_dir.empty())
        home_dir = default_home_dir();
    if ( !std::filesystem::exists(home_dir))
        std::filesystem::create_directories(home_dir);
}

} // namespace agent
