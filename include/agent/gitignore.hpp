#pragma once

#include <string>
#include <vector>

namespace agent {

// A pragmatic .gitignore matcher for the search tools (~90% of real-world
// patterns: names, globs, directory-only `dir/`, root-anchored `/x`, `**/x`,
// and `!` negation with last-match-wins). Not full git semantics — nested
// per-directory .gitignore files and exotic `a/**/b` cases are out of scope.
class GitIgnore {
public:
    // Load the .gitignore at `root` (and .git/info/exclude if present). Paths
    // passed to ignored() are then interpreted relative to `root`.
    void load(const std::string& root);

    bool empty() const { return _rules.empty(); }

    // Is `relpath` (relative to root, '/'-separated, no leading "./") ignored?
    // is_dir lets directory-only patterns (`build/`) apply. During a walk, when
    // this returns true for a directory, don't descend into it.
    bool ignored(const std::string& relpath, bool is_dir) const;

private:
    struct Rule {
        std::string glob;   // the pattern (leading '!', trailing '/', anchoring stripped)
        bool negated = false;
        bool dir_only = false;
        bool anchored = false;   // has an internal/leading '/', matched against the full relpath
        bool basename_star = false; // "**/x" form — match the basename against `glob`
    };
    std::vector<Rule> _rules;

    void add_pattern(std::string line);
};

} // namespace agent
