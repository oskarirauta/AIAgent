#pragma once

#include <string>
#include <vector>

namespace agent {

std::string load_memories(const std::string& home_dir, const std::string& provider);

struct MemoryFile {
    std::string name;
    size_t bytes = 0;
    size_t lines = 0;
};

// List the memory files for a provider (sorted by name).
std::vector<MemoryFile> list_memories(const std::string& home_dir, const std::string& provider);

// Read one memory file's content. Returns empty if the name is unsafe (contains
// a path separator or "..") or the file does not exist / is not a memory file.
std::string read_memory(const std::string& home_dir, const std::string& provider, const std::string& name);

} // namespace agent
