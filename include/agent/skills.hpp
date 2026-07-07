#pragma once

#include <string>
#include <vector>

namespace agent {

// A named, reusable instruction set beyond AGENTS.md: a markdown file with
// optional frontmatter (name, description) and an instructions body. Loaded
// from <home>/skills/*.md (user) and <cwd>/.agent/skills/*.md (project).
struct Skill {
    std::string name;        // slug (frontmatter `name:` or the filename stem)
    std::string description; // one-line summary, shown to the user and the model
    std::string content;     // the instructions body (frontmatter stripped)
    std::string source;      // "user" or "project"
    std::string path;        // absolute file path
};

// Discover skills from the user (<home>/skills) and project (<cwd>/.agent/skills)
// directories. Project skills override a user skill with the same name. Sorted
// by name.
std::vector<Skill> load_skills(const std::string& home_dir, const std::string& project_dir);

} // namespace agent
