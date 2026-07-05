#include "agent/memory.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include "common.hpp"
#include "logger.hpp"

namespace agent {

static bool is_memory_file(const std::string& name) {
    if ( name.empty() || name.front() == '.' )
        return false;
    if ( name == "README" || name == "README.md" )
        return true;
    if ( common::has_suffix(name, ".md") || common::has_suffix(name, ".txt") || common::has_suffix(name, ".md") ||
         common::has_suffix(name, ".memory") || common::has_suffix(name, ".prompt"))
        return true;
    // files without extension are also allowed
    if ( name.find('.') == std::string::npos )
        return true;
    return false;
}

std::string load_memories(const std::string& home_dir, const std::string& provider) {

    // Memories are per-provider (but not per-model), so switching e.g. Claude
    // Opus -> Claude Fable keeps the same memory set while Claude and Kimi stay
    // separate.
    std::string mem_dir = home_dir + "/memories/" + provider;
    if ( !std::filesystem::exists(mem_dir) || !std::filesystem::is_directory(mem_dir)) {
        logger::verbose["memory"] << "no memories directory: " << mem_dir << std::endl;
        return "";
    }

    std::vector<std::string> files;
    for ( const auto& entry : std::filesystem::directory_iterator(mem_dir)) {
        if ( !entry.is_regular_file())
            continue;
        std::string name = entry.path().filename().string();
        if ( is_memory_file(name))
            files.push_back(name);
    }

    if ( files.empty()) {
        logger::verbose["memory"] << "no memory files found" << std::endl;
        return "";
    }

    std::sort(files.begin(), files.end());

    std::ostringstream oss;
    oss << "\n\n## Long-term memory\n\n";

    for ( const auto& name : files ) {
        std::string path = mem_dir + "/" + name;
        std::ifstream ifd(path, std::ios::in);
        if ( !ifd.is_open()) {
            logger::warning["memory"] << "cannot read memory file: " << path << std::endl;
            continue;
        }

        std::stringstream ss;
        ss << ifd.rdbuf();
        std::string content = ss.str();

        if ( content.empty())
            continue;

        oss << "### " << name << "\n\n" << content << "\n\n";
    }

    std::string result = oss.str();
    logger::info["memory"] << "loaded " << files.size() << " memory file(s), " << result.size() << " chars" << std::endl;
    return result;
}

} // namespace agent
