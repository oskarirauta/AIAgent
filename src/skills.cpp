#include "agent/skills.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <map>
#include "common.hpp"

namespace agent {

namespace {

constexpr size_t MAX_SKILL_BYTES = 64 * 1024;

// Parse optional YAML-ish frontmatter (--- … ---) for name/description, and
// return the body with the frontmatter stripped.
void parse_skill(const std::string& raw, const std::string& stem,
                 std::string& name, std::string& description, std::string& body) {
    name = stem;
    description = "";
    body = raw;

    std::istringstream is(raw);
    std::string first;
    if ( !std::getline(is, first))
        return;
    if ( common::trim_ws(first) != "---" )
        return; // no frontmatter

    std::string line;
    std::string front;
    bool closed = false;
    while ( std::getline(is, line)) {
        if ( common::trim_ws(line) == "---" ) { closed = true; break; }
        front += line + "\n";
    }
    if ( !closed )
        return; // malformed — treat the whole file as body

    // Body is everything after the closing '---'.
    std::ostringstream rest;
    rest << is.rdbuf();
    body = rest.str();
    // Drop a single leading blank line.
    if ( !body.empty() && body[0] == '\n' )
        body.erase(0, 1);

    std::istringstream fs(front);
    while ( std::getline(fs, line)) {
        size_t colon = line.find(':');
        if ( colon == std::string::npos )
            continue;
        std::string key = common::to_lower(common::trim_ws(line.substr(0, colon)));
        std::string val = common::trim_ws(line.substr(colon + 1));
        if ( key == "name" && !val.empty()) name = val;
        else if ( key == "description" ) description = val;
    }
}

void scan_dir(const std::string& dir, const std::string& source,
              std::map<std::string, Skill>& out) {
    std::error_code ec;
    if ( !std::filesystem::is_directory(dir, ec))
        return;
    for ( const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code fe;
        if ( !e.is_regular_file(fe))
            continue;
        if ( e.path().extension() != ".md" )
            continue;
        if ( std::filesystem::file_size(e.path(), fe) > MAX_SKILL_BYTES )
            continue;
        std::ifstream ifd(e.path(), std::ios::in | std::ios::binary);
        if ( !ifd.is_open())
            continue;
        std::stringstream ss; ss << ifd.rdbuf();

        Skill s;
        parse_skill(ss.str(), e.path().stem().string(), s.name, s.description, s.content);
        if ( common::trim_ws(s.content).empty())
            continue;
        s.source = source;
        s.path = e.path().string();
        out[s.name] = s; // project (scanned second) overrides user with same name
    }
}

} // namespace

std::vector<Skill> load_skills(const std::string& home_dir, const std::string& project_dir) {
    std::map<std::string, Skill> byname;
    scan_dir(home_dir + "/skills", "user", byname);
    scan_dir(project_dir + "/.agent/skills", "project", byname);

    std::vector<Skill> out;
    out.reserve(byname.size());
    for ( auto& [name, s] : byname )
        out.push_back(std::move(s));
    std::sort(out.begin(), out.end(), [](const Skill& a, const Skill& b) { return a.name < b.name; });
    return out;
}

} // namespace agent
