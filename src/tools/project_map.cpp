#include "agent/tools/project_map.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <regex>
#include "common.hpp"
#include "json.hpp"

namespace agent::tools {

namespace fs = std::filesystem;

namespace {

constexpr size_t MAX_FILES_SCANNED = 40000;

bool ignored_dir(const std::string& name) {
    static const std::vector<std::string> skip = {
        ".git", ".svn", ".hg", "node_modules", "objs", "build", "dist",
        "target", ".cache", "vendor", "third_party", ".venv", "venv",
        "__pycache__", ".idea", ".vscode", "bin", "obj"
    };
    for ( const auto& s : skip )
        if ( name == s )
            return true;
    return false;
}

std::string read_file(const fs::path& p) {
    std::ifstream ifd(p, std::ios::in | std::ios::binary);
    if ( !ifd.is_open())
        return "";
    std::stringstream ss; ss << ifd.rdbuf();
    return ss.str();
}

std::string trim(const std::string& s) { return common::trim_ws(s); }

// package.json / composer.json: name, scripts, dependency counts.
std::string summarize_json_manifest(const std::string& content) {
    try {
        JSON j = JSON::parse(content);
        std::string out;
        if ( j.contains("name") && j["name"] == JSON::TYPE::STRING )
            out += " name \"" + j["name"].to_string() + "\"";
        if ( j.contains("scripts") && j["scripts"] == JSON::TYPE::OBJECT ) {
            std::vector<std::string> names;
            j["scripts"].for_each([&names](JSON::fe_iterator& it, const JSON&) {
                if ( it.is_object()) names.push_back(it.name());
            });
            if ( !names.empty()) {
                out += ", scripts: ";
                for ( size_t i = 0; i < names.size() && i < 8; ++i )
                    out += ( i ? ", " : "" ) + names[i];
                if ( names.size() > 8 ) out += ", …";
            }
        }
        auto count = [&](const char* key) -> size_t {
            if ( j.contains(key) && j[key] == JSON::TYPE::OBJECT ) {
                size_t n = 0;
                j[key].for_each([&n](JSON::fe_iterator& it, const JSON&) { if ( it.is_object()) ++n; });
                return n;
            }
            return 0;
        };
        size_t deps = count("dependencies"), dev = count("devDependencies");
        if ( deps || dev )
            out += ", " + std::to_string(deps) + " deps" + ( dev ? ( " + " + std::to_string(dev) + " dev" ) : "" );
        return out;
    } catch ( ... ) {
        return "";
    }
}

// TOML-ish (Cargo.toml / pyproject.toml): name = "…" and a [dependencies] count.
std::string summarize_toml(const std::string& content) {
    std::string out;
    std::smatch m;
    std::regex name_re("(^|\\n)\\s*name\\s*=\\s*\"([^\"]+)\"");
    if ( std::regex_search(content, m, name_re))
        out += " name \"" + m[2].str() + "\"";

    // Count entries under a [dependencies] (or [tool.poetry.dependencies]) table.
    std::istringstream is(content);
    std::string line;
    bool in_deps = false;
    size_t deps = 0;
    while ( std::getline(is, line)) {
        std::string t = trim(line);
        if ( !t.empty() && t[0] == '[' ) {
            in_deps = ( t.find("dependencies") != std::string::npos && t.find("dev") == std::string::npos );
            continue;
        }
        if ( in_deps && !t.empty() && t[0] != '#' && t.find('=') != std::string::npos )
            ++deps;
    }
    if ( deps )
        out += ", " + std::to_string(deps) + " deps";
    return out;
}

std::string summarize_gomod(const std::string& content) {
    std::string out;
    std::smatch m;
    if ( std::regex_search(content, m, std::regex("(^|\\n)module\\s+(\\S+)")))
        out += " module " + m[2].str();
    if ( std::regex_search(content, m, std::regex("(^|\\n)go\\s+(\\S+)")))
        out += ", go " + m[2].str();
    size_t reqs = 0;
    std::istringstream is(content);
    std::string line;
    bool block = false;
    while ( std::getline(is, line)) {
        std::string t = trim(line);
        if ( t.rfind("require (", 0) == 0 ) { block = true; continue; }
        if ( block && t == ")" ) { block = false; continue; }
        if ( block && !t.empty()) ++reqs;
        else if ( t.rfind("require ", 0) == 0 ) ++reqs;
    }
    if ( reqs )
        out += ", " + std::to_string(reqs) + " requires";
    return out;
}

std::string summarize_makefile(const std::string& content) {
    std::vector<std::string> targets;
    std::istringstream is(content);
    std::string line;
    std::regex tgt("^([A-Za-z0-9_][A-Za-z0-9_./-]*)\\s*:($|[^=])");
    while ( std::getline(is, line)) {
        std::smatch m;
        if ( std::regex_search(line, m, tgt)) {
            std::string name = m[1].str();
            // Skip pattern/object/path targets — keep the meaningful named ones.
            if ( name.empty() || name[0] == '.' ) continue;
            if ( name.find('/') != std::string::npos ) continue;
            if ( name.size() > 2 && ( name.substr(name.size() - 2) == ".o" ||
                                      name.substr(name.size() - 2) == ".d" )) continue;
            if ( std::find(targets.begin(), targets.end(), name) == targets.end())
                targets.push_back(name);
        }
    }
    if ( targets.empty())
        return "";
    std::string out = " targets: ";
    for ( size_t i = 0; i < targets.size() && i < 12; ++i )
        out += ( i ? ", " : "" ) + targets[i];
    if ( targets.size() > 12 ) out += ", …";
    return out;
}

std::string summarize_cmake(const std::string& content) {
    std::smatch m;
    if ( std::regex_search(content, m, std::regex("project\\s*\\(\\s*([A-Za-z0-9_.-]+)", std::regex::icase)))
        return " project " + m[1].str();
    return "";
}

} // namespace

JSON ProjectMap::parameters() const {
    return JSON::Object{
        { "type", "object" },
        { "properties", JSON::Object{
            { "path", JSON::Object{
                { "type", "string" },
                { "description", "project root to map (optional, defaults to the working directory)" }
            }}
        }},
        { "required", JSON::Array{} }
    };
}

std::string ProjectMap::execute(const JSON& args) {
    std::string root = ".";
    if ( args.contains("path")) {
        std::string p = common::trim_ws(args["path"].to_string());
        if ( !p.empty())
            root = p;
    }
    std::error_code ec;
    if ( !fs::is_directory(root, ec))
        return "error: not a directory: " + root;

    std::string out = "project map for " + root + ":\n";

    // ── manifests in the root ────────────────────────────────────────────
    struct Manifest { const char* file; std::string (*fn)(const std::string&); };
    static const std::vector<Manifest> manifests = {
        { "package.json", summarize_json_manifest }, { "composer.json", summarize_json_manifest },
        { "Cargo.toml", summarize_toml }, { "pyproject.toml", summarize_toml },
        { "go.mod", summarize_gomod }, { "Makefile", summarize_makefile },
        { "makefile", summarize_makefile }, { "CMakeLists.txt", summarize_cmake }
    };
    std::string man_out;
    static const std::vector<std::string> plain = {
        "requirements.txt", "setup.py", "setup.cfg", "Gemfile", "pom.xml",
        "build.gradle", "build.gradle.kts", ".csproj", "meson.build", "configure.ac"
    };
    for ( const auto& m : manifests ) {
        fs::path f = fs::path(root) / m.file;
        if ( fs::is_regular_file(f, ec)) {
            std::string summary = m.fn(read_file(f));
            man_out += "  " + std::string(m.file) + summary + "\n";
        }
    }
    for ( const auto& name : plain ) {
        fs::path f = fs::path(root) / name;
        if ( fs::is_regular_file(f, ec))
            man_out += "  " + name + "\n";
    }
    if ( !man_out.empty())
        out += "\nbuild / manifest files:\n" + man_out;

    // ── top-level layout + per-directory file counts + language histogram ──
    std::map<std::string, size_t> dir_files;   // top-level dir -> recursive file count
    std::vector<std::string> top_dirs, top_files;
    std::map<std::string, size_t> ext_counts;
    size_t scanned = 0;
    bool capped = false;

    for ( const auto& e : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
        std::string name = e.path().filename().string();
        std::error_code de;
        if ( e.is_directory(de)) {
            if ( name[0] == '.' || ignored_dir(name)) continue;
            top_dirs.push_back(name);
        } else if ( e.is_regular_file(de)) {
            top_files.push_back(name);
        }
    }
    std::sort(top_dirs.begin(), top_dirs.end());
    std::sort(top_files.begin(), top_files.end());

    auto ext_of = [](const fs::path& p) {
        std::string e = p.extension().string();
        return e.empty() ? std::string("(none)") : e;
    };
    // Count top-level files' extensions too.
    for ( const auto& f : top_files )
        ext_counts[ext_of(fs::path(f))]++;

    for ( const auto& d : top_dirs ) {
        size_t count = 0;
        fs::recursive_directory_iterator it(fs::path(root) / d,
            fs::directory_options::skip_permission_denied, ec), end;
        for ( ; it != end; it.increment(ec)) {
            if ( ec || scanned >= MAX_FILES_SCANNED ) { capped = true; break; }
            std::error_code fe;
            if ( it->is_directory(fe)) {
                if ( ignored_dir(it->path().filename().string()))
                    it.disable_recursion_pending();
                continue;
            }
            if ( it->is_regular_file(fe)) {
                ++count; ++scanned;
                ext_counts[ext_of(it->path())]++;
            }
        }
        dir_files[d] = count;
    }

    out += "\nstructure:\n";
    for ( const auto& d : top_dirs )
        out += "  " + d + "/  (" + std::to_string(dir_files[d]) +
               ( dir_files[d] == 1 ? " file)" : " files)" ) + "\n";
    if ( !top_files.empty()) {
        out += "  top-level files: ";
        for ( size_t i = 0; i < top_files.size() && i < 20; ++i )
            out += ( i ? ", " : "" ) + top_files[i];
        if ( top_files.size() > 20 ) out += ", …";
        out += "\n";
    }

    // Language histogram: top extensions by count.
    std::vector<std::pair<std::string, size_t>> exts(ext_counts.begin(), ext_counts.end());
    std::sort(exts.begin(), exts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if ( !exts.empty()) {
        out += "\nfiles by type: ";
        for ( size_t i = 0; i < exts.size() && i < 10; ++i )
            out += ( i ? ", " : "" ) + std::to_string(exts[i].second) + " " + exts[i].first;
        out += "\n";
    }
    if ( capped )
        out += "\n(large tree — file counts stopped at the scan limit)\n";

    return out;
}

} // namespace agent::tools
