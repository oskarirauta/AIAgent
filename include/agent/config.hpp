#pragma once

#include <string>
#include <optional>
#include "usage.hpp"

namespace agent {

class Config {
public:
    std::string provider = "openai";
    std::string model = "gpt-4o-mini";
    std::string api_url = "https://api.openai.com/v1";
    std::string api_key;
    std::string log_level = "info";
    std::string system_prompt = "You are a helpful Linux CLI assistant.";
    std::string home_dir;
    bool tools_enabled = true;
    bool confirm_tools = true;

    void load(const std::string& path);
    void apply_cli(const usage_t& usage);
    void ensure_home_dir();

    static std::string default_path();
    static std::string default_home_dir();
};

} // namespace agent
