#include "agent/gitignore.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <fnmatch.h>
#include "common.hpp"

namespace agent {

namespace {

std::string basename_of(const std::string& p) {
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

} // namespace

void GitIgnore::add_pattern(std::string line) {
    // Trim trailing whitespace that is not escaped, and skip blanks/comments.
    while ( !line.empty() && ( line.back() == ' ' || line.back() == '\t' ||
                               line.back() == '\r' || line.back() == '\n' ))
        line.pop_back();
    if ( line.empty() || line[0] == '#' )
        return;

    Rule r;
    if ( line[0] == '!' ) { r.negated = true; line.erase(0, 1); }
    if ( line.empty()) return;
    if ( line.back() == '/' ) { r.dir_only = true; line.pop_back(); }
    if ( line.empty()) return;

    if ( line.rfind("**/", 0) == 0 ) {
        // "**/x" — match x by basename at any depth.
        r.basename_star = true;
        r.glob = line.substr(3);
    } else {
        bool leading_slash = ( line[0] == '/' );
        if ( leading_slash )
            line.erase(0, 1);
        // A '/' anywhere (or a stripped leading '/') anchors the pattern to the
        // root; otherwise it matches by basename at any depth.
        r.anchored = leading_slash || ( line.find('/') != std::string::npos );
        r.glob = line;
    }
    if ( !r.glob.empty())
        _rules.push_back(std::move(r));
}

void GitIgnore::load(const std::string& root) {
    _rules.clear();
    auto read = [&](const std::string& path) {
        std::ifstream ifd(path, std::ios::in);
        if ( !ifd.is_open())
            return;
        std::string line;
        while ( std::getline(ifd, line))
            add_pattern(line);
    };
    read(root + "/.gitignore");
    read(root + "/.git/info/exclude");
    // .git itself is always uninteresting to the search tools.
    add_pattern(".git/");
}

bool GitIgnore::ignored(const std::string& relpath, bool is_dir) const {
    if ( relpath.empty())
        return false;
    std::string base = basename_of(relpath);

    bool result = false;
    for ( const auto& r : _rules ) {
        if ( r.dir_only && !is_dir )
            continue;
        bool m = false;
        if ( r.basename_star ) {
            m = fnmatch(r.glob.c_str(), base.c_str(), 0) == 0;
        } else if ( r.anchored ) {
            m = fnmatch(r.glob.c_str(), relpath.c_str(), FNM_PATHNAME) == 0;
        } else {
            // Match by basename at any depth (the common `*.o`, `node_modules`).
            m = fnmatch(r.glob.c_str(), base.c_str(), 0) == 0;
        }
        if ( m )
            result = !r.negated; // last matching rule wins
    }
    return result;
}

} // namespace agent
