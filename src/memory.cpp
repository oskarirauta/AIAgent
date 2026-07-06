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
    if ( common::has_suffix(name, ".md") || common::has_suffix(name, ".txt") ||
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

// Project-instructions filenames, in priority order. AGENTS.md is the emerging
// cross-tool convention; .ai-agent.md / AGENT.md are accepted too.
static const std::vector<std::string>& project_instruction_names() {
    static const std::vector<std::string> names = { "AGENTS.md", ".ai-agent.md", "AGENT.md" };
    return names;
}

std::string project_instructions_file(const std::string& dir) {
    for ( const auto& name : project_instruction_names()) {
        std::error_code ec;
        std::string path = dir + "/" + name;
        if ( std::filesystem::is_regular_file(path, ec))
            return name;
    }
    return "";
}

std::string load_project_instructions(const std::string& dir) {
    std::string name = project_instructions_file(dir);
    if ( name.empty())
        return "";

    std::ifstream ifd(dir + "/" + name, std::ios::in);
    if ( !ifd.is_open())
        return "";
    std::stringstream ss;
    ss << ifd.rdbuf();
    std::string content = ss.str();

    // Cap the size so a stray large file can't swamp the context window.
    const size_t cap = 64 * 1024;
    if ( content.size() > cap )
        content = content.substr(0, cap) + "\n\n[project instructions truncated]";

    // Trim trailing whitespace so the block ends cleanly.
    while ( !content.empty() && ( content.back() == '\n' || content.back() == ' ' ||
                                  content.back() == '\t' || content.back() == '\r' ))
        content.pop_back();
    if ( content.empty())
        return "";

    logger::info["memory"] << "loaded project instructions from " << name
                           << " (" << content.size() << " chars)" << std::endl;
    return "\n\n## Project instructions (from " + name + ")\n\n" + content + "\n";
}

std::vector<MemoryFile> list_memories(const std::string& home_dir, const std::string& provider) {
    std::vector<MemoryFile> out;
    std::string mem_dir = home_dir + "/memories/" + provider;
    if ( !std::filesystem::exists(mem_dir) || !std::filesystem::is_directory(mem_dir))
        return out;

    for ( const auto& entry : std::filesystem::directory_iterator(mem_dir)) {
        if ( !entry.is_regular_file())
            continue;
        std::string name = entry.path().filename().string();
        if ( !is_memory_file(name))
            continue;
        MemoryFile mf;
        mf.name = name;
        std::ifstream ifd(entry.path(), std::ios::in);
        if ( ifd.is_open()) {
            std::stringstream ss;
            ss << ifd.rdbuf();
            std::string content = ss.str();
            mf.bytes = content.size();
            mf.lines = static_cast<size_t>(std::count(content.begin(), content.end(), '\n'));
            if ( !content.empty() && content.back() != '\n' )
                ++mf.lines;
        }
        out.push_back(mf);
    }
    std::sort(out.begin(), out.end(), [](const MemoryFile& a, const MemoryFile& b) { return a.name < b.name; });
    return out;
}

std::string read_memory(const std::string& home_dir, const std::string& provider, const std::string& name) {
    // Reject anything that could escape the provider's memory directory.
    if ( name.empty() || name.find('/') != std::string::npos ||
         name.find('\\') != std::string::npos || name.find("..") != std::string::npos )
        return "";
    if ( !is_memory_file(name))
        return "";

    std::string path = home_dir + "/memories/" + provider + "/" + name;
    if ( !std::filesystem::exists(path) || !std::filesystem::is_regular_file(path))
        return "";

    std::ifstream ifd(path, std::ios::in);
    if ( !ifd.is_open())
        return "";
    std::stringstream ss;
    ss << ifd.rdbuf();
    return ss.str();
}

} // namespace agent
