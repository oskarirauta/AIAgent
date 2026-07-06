#pragma once

#include <string>
#include <vector>

namespace agent {

std::string load_memories(const std::string& home_dir, const std::string& provider);

// Look in `dir` (the working directory) for a project-instructions file
// (AGENTS.md, .ai-agent.md, AGENT.md — first match wins) and return its basename,
// or empty if none exists.
std::string project_instructions_file(const std::string& dir);

// Load the project-instructions file's content as a system-prompt block (capped
// in size), or empty if there is none. Lets a project pin coding style, testing
// conventions, etc. without repeating them each session.
std::string load_project_instructions(const std::string& dir);

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
