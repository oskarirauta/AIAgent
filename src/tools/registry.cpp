#include "agent/tools/registry.hpp"

#include <sstream>
#include <vector>
#include <algorithm>
#include <filesystem>
#include "throws.hpp"
#include "logger.hpp"
#include "common.hpp"
#include "agent/tools/read_file.hpp"
#include "agent/tools/write_file.hpp"
#include "agent/tools/edit_file.hpp"
#include "agent/tools/run_command.hpp"
#include "agent/tools/list_directory.hpp"
#include "agent/tools/grep.hpp"
#include "agent/tools/find_symbol.hpp"
#include "agent/tools/find_references.hpp"

namespace agent::tools {

void Registry::register_defaults() {
    add(std::make_unique<ReadFile>());
    add(std::make_unique<WriteFile>());
    add(std::make_unique<EditFile>());
    add(std::make_unique<RunCommand>());
    add(std::make_unique<ListDirectory>());
    add(std::make_unique<Grep>());
    add(std::make_unique<FindSymbol>());
    add(std::make_unique<FindReferences>());
}

void Registry::add(std::unique_ptr<Tool> tool) {
    _tools[tool->name()] = std::move(tool);
}

void Registry::remove(const std::string& name) {
    _tools.erase(name);
}

void Registry::set_confirm_callback(confirm_cb_t cb) {
    _confirm_cb = std::move(cb);
}

JSON Registry::schema() const {
    JSON arr = JSON::Array{};
    for ( const auto& [name, tool] : _tools ) {
        JSON entry = JSON::Object{
            { "type", "function" },
            { "function", JSON::Object{
                { "name", tool->name() },
                { "description", tool->description() },
                { "parameters", tool->parameters() }
            }}
        };
        arr.append(entry);
    }
    return arr;
}

// ── danger list ─────────────────────────────────────────────────────────

namespace {

std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string tok;
    while ( iss >> tok )
        out.push_back(tok);
    return out;
}

std::string basename_of(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A danger rule matches when the program equals `program` (basename) and, if
// `any_flags` is non-empty, at least one of those tokens/substrings is present.
struct DangerRule {
    std::string program;
    std::vector<std::string> any_flags; // empty => always dangerous for this program
    std::string reason;
};

const std::vector<DangerRule>& danger_rules() {
    static const std::vector<DangerRule> rules = {
        { "rm",       { "-r", "-rf", "-fr", "-R", "-f", "--recursive", "--force", "/", "*" }, "deletes files (recursive/forced or wildcard)" },
        { "rmdir",    { "/" },                                   "removes directories" },
        { "dd",       {},                                        "raw disk write — can destroy data" },
        { "mkfs",     {},                                        "formats a filesystem" },
        { "fdisk",    {},                                        "edits the partition table" },
        { "parted",   {},                                        "edits the partition table" },
        { "shutdown", {},                                        "powers down the system" },
        { "reboot",   {},                                        "reboots the system" },
        { "halt",     {},                                        "halts the system" },
        { "poweroff", {},                                        "powers off the system" },
        { "sudo",     {},                                        "runs a command as root" },
        { "doas",     {},                                        "runs a command as root" },
        { "su",       {},                                        "switches user / privilege" },
        { "passwd",   {},                                        "changes an account password" },
        { "chpasswd", {},                                        "changes account passwords" },
        { "usermod",  {},                                        "modifies a user account" },
        { "userdel",  {},                                        "deletes a user account" },
        { "chmod",    { "-R", "--recursive", "777" },            "recursive or world-writable permission change" },
        { "chown",    { "-R", "--recursive" },                   "recursive ownership change" },
        { "git",      { "push" },                                "pushes to a remote (verify branch/force)" },
        { "kill",     { "-9" },                                  "force-kills a process" },
        { "killall",  {},                                        "kills processes by name" },
        { "mkfs.ext4",{},                                        "formats a filesystem" },
    };
    return rules;
}

} // namespace

std::string Registry::classify_danger(const std::string& command) {
    std::vector<std::string> tokens = tokenize(command);
    if ( tokens.empty())
        return "";

    std::string program = basename_of(tokens[0]);

    // Pipe-into-shell: fetching remote content and executing it.
    std::string lower = common::to_lower(command);
    if ( (lower.find("curl") != std::string::npos || lower.find("wget") != std::string::npos) &&
         (lower.find("| sh") != std::string::npos || lower.find("|sh") != std::string::npos ||
          lower.find("| bash") != std::string::npos || lower.find("|bash") != std::string::npos)) {
        return "pipes downloaded content straight into a shell";
    }

    // Fork bomb.
    if ( command.find(":(){") != std::string::npos )
        return "looks like a fork bomb";

    // Sensitive system files / block devices, referenced by any command.
    static const std::vector<std::string> sensitive_exact = {
        "/etc/passwd", "/etc/shadow", "/etc/gshadow", "/etc/group",
        "/etc/sudoers", "/etc/fstab", "/etc/hosts"
    };
    static const std::vector<std::string> sensitive_prefix = {
        "/dev/sd", "/dev/nvme", "/dev/hd", "/dev/mapper/", "/dev/disk/",
        "/etc/sudoers.d/", "/boot/"
    };
    for ( size_t i = 1; i < tokens.size(); ++i ) {
        for ( const auto& p : sensitive_exact )
            if ( tokens[i] == p )
                return "touches a sensitive system file (" + p + ")";
        for ( const auto& p : sensitive_prefix )
            if ( tokens[i].rfind(p, 0) == 0 )
                return "touches a sensitive system path (" + p + "…)";
    }

    for ( const auto& rule : danger_rules()) {
        if ( rule.program != program )
            continue;
        if ( rule.any_flags.empty())
            return rule.reason;
        for ( const auto& flag : rule.any_flags ) {
            // Option flags (-r, -rf, …) substring-match so clusters like -rfv hit;
            // everything else (/, *, 777) must be its own token, so a path like
            // /tmp/x or a name like file777 is not mistaken for the criterion.
            bool is_option = !flag.empty() && flag[0] == '-';
            for ( size_t i = 1; i < tokens.size(); ++i ) {
                bool hit = is_option ? ( tokens[i].find(flag) != std::string::npos )
                                     : ( tokens[i] == flag );
                if ( hit )
                    return rule.reason;
            }
        }
    }

    return "";
}

std::string Registry::classify_path_danger(const std::string& path) {
    if ( path.empty())
        return "";

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    fs::path target(path);
    fs::path abs = target.is_absolute() ? target : (cwd / target);
    std::string s = abs.lexically_normal().string();
    if ( s.size() > 1 && s.back() == '/' )
        s.pop_back();

    // Paths under the working directory (the project) or /tmp (conventional
    // scratch area) are safe — checked first, since the project itself may live
    // under a system prefix like /usr/src.
    std::string cwds = cwd.lexically_normal().string();
    if ( cwds.size() > 1 && cwds.back() == '/' )
        cwds.pop_back();
    if ( s == cwds || s.rfind(cwds + "/", 0) == 0 ||
         s == "/tmp" || s.rfind("/tmp/", 0) == 0 )
        return "";

    // Otherwise, a system directory gets a specific reason.
    static const std::vector<std::string> sysdirs = {
        "/proc", "/sys", "/dev", "/etc", "/boot", "/bin", "/sbin",
        "/lib", "/lib64", "/usr", "/var", "/root"
    };
    for ( const auto& d : sysdirs ) {
        if ( s == d || s.rfind(d + "/", 0) == 0 )
            return "writes into a system directory (" + d + ")";
    }

    return "writes outside the working directory";
}

// A single simple command (no connectors). Redirection, background, subshell
// and stray pipes are rejected; otherwise the program decides.
static bool classify_safe_simple(const std::string& command) {
    for ( char c : command ) {
        if ( c == '>' || c == '<' || c == '|' || c == ';' || c == '&' ||
             c == '`' || c == '\n' || c == '\r' )
            return false;
    }
    if ( command.find("$(") != std::string::npos )
        return false;

    std::vector<std::string> tokens = tokenize(command);
    if ( tokens.empty())
        return false;
    std::string prog = basename_of(tokens[0]);

    // Read-only commands with no side effects.
    static const std::vector<std::string> safe = {
        "date", "cal", "pwd", "whoami", "hostname", "id", "uname", "which",
        "ls", "cat", "head", "tail", "df", "free", "echo", "printenv", "env",
        "wc", "file", "stat", "true", "false", "tty", "groups", "uptime",
        "arch", "nproc", "basename", "dirname", "realpath", "readlink", "locale",
        "pkg-config", "type", "whereis",
        // `cd` only affects the command's own subshell; these filters read stdin
        // / files and write only to stdout, so they are safe inside a pipe.
        "cd", "grep", "egrep", "fgrep", "tr", "cut", "rev", "tac", "nl"
    };
    if ( std::find(safe.begin(), safe.end(), prog) != safe.end()) {
        // `tail -f` / `--follow` would block until the timeout — not classified
        // safe so it still asks before tying up the turn.
        if ( prog == "tail" ) {
            for ( size_t i = 1; i < tokens.size(); ++i )
                if ( tokens[i] == "-f" || tokens[i] == "-F" || tokens[i] == "--follow" )
                    return false;
        }
        return true;
    }

    // `command -v/-V NAME` (a lookup, not an execution).
    if ( prog == "command" ) {
        for ( size_t i = 1; i < tokens.size(); ++i )
            if ( tokens[i] == "-v" || tokens[i] == "-V" )
                return true;
        return false;
    }

    // Compilers/toolchain: safe only in a version/info form (no sources/output).
    static const std::vector<std::string> compilers = {
        "gcc", "g++", "clang", "clang++", "cc", "c++", "cpp", "ld", "as"
    };
    if ( std::find(compilers.begin(), compilers.end(), prog) != compilers.end()) {
        static const std::vector<std::string> info_flags = {
            "-v", "--version", "-V", "-dumpversion", "-dumpmachine",
            "-dumpfullversion", "-dumpspecs", "--help", "-print-search-dirs"
        };
        if ( tokens.size() < 2 )
            return false; // bare `gcc` reads stdin
        for ( size_t i = 1; i < tokens.size(); ++i )
            if ( std::find(info_flags.begin(), info_flags.end(), tokens[i]) == info_flags.end())
                return false;
        return true;
    }

    // git: read-only subcommands only.
    if ( prog == "git" ) {
        if ( tokens.size() < 2 )
            return false;
        const std::string& sub = tokens[1];
        // These only read, so any arguments are fine.
        static const std::vector<std::string> ro_any = {
            "status", "log", "diff", "show", "rev-parse", "ls-files", "ls-tree",
            "blame", "shortlog", "whatchanged", "cat-file", "for-each-ref",
            "reflog", "describe"
        };
        if ( std::find(ro_any.begin(), ro_any.end(), sub) != ro_any.end())
            return true;
        // These list when given no positional argument, but create/modify when
        // given one (e.g. `git branch foo` creates a branch) — safe only as a
        // listing (flags allowed, no bare argument, no mutating flag).
        static const std::vector<std::string> listing = { "branch", "tag", "remote" };
        if ( std::find(listing.begin(), listing.end(), sub) != listing.end()) {
            static const std::vector<std::string> mutating = {
                "-d", "-D", "-m", "-M", "-f", "--delete", "--force", "--move",
                "--add", "--set", "--unset", "--edit", "--create", "--set-url",
                "--prune", "--set-head", "-c", "-C"
            };
            for ( size_t i = 2; i < tokens.size(); ++i ) {
                if ( tokens[i].empty() || tokens[i][0] != '-' )
                    return false; // a positional argument names something to create/modify
                if ( std::find(mutating.begin(), mutating.end(), tokens[i]) != mutating.end())
                    return false;
            }
            return true;
        }
        return false;
    }

    // make: safe only in a dry-run / query / version form (bare `make` runs).
    if ( prog == "make" ) {
        static const std::vector<std::string> dry = {
            "-n", "--dry-run", "--just-print", "--recon",
            "-p", "--print-data-base", "-q", "--question", "--version", "--help"
        };
        for ( size_t i = 1; i < tokens.size(); ++i )
            if ( std::find(dry.begin(), dry.end(), tokens[i]) != dry.end())
                return true;
        return false;
    }

    // Any program invoked purely to print its version / help is harmless.
    if ( tokens.size() == 2 &&
         ( tokens[1] == "--version" || tokens[1] == "--help" || tokens[1] == "-version" ))
        return true;

    return false;
}

bool Registry::classify_safe(const std::string& command) {
    // A subshell anywhere makes the whole thing unvouchable.
    if ( command.find("$(") != std::string::npos )
        return false;

    // Split on the sequencing/pipe connectors (&&, ||, ;, |) and require every
    // segment to be individually safe, so chains like `cd dir && git log` or
    // `git log | grep foo` are recognised. A lone `&` (background) or a redirect
    // stays inside a segment and is rejected by classify_safe_simple.
    std::vector<std::string> parts;
    std::string cur;
    for ( size_t i = 0; i < command.size(); ) {
        if ( command[i] == '&' && i + 1 < command.size() && command[i + 1] == '&' ) {
            parts.push_back(cur); cur.clear(); i += 2;
        } else if ( command[i] == '|' && i + 1 < command.size() && command[i + 1] == '|' ) {
            parts.push_back(cur); cur.clear(); i += 2;
        } else if ( command[i] == ';' || command[i] == '|' ) {
            parts.push_back(cur); cur.clear(); i += 1;
        } else {
            cur += command[i]; ++i;
        }
    }
    parts.push_back(cur);

    for ( const std::string& p : parts ) {
        std::string seg = common::trim_ws(p);
        if ( seg.empty() || !classify_safe_simple(seg))
            return false;
    }
    return true;
}

// ── execution with confirmation policy ──────────────────────────────────

std::string Registry::execute(const std::string& name, const JSON& args) {
    auto it = _tools.find(name);
    if ( it == _tools.end())
        throws << "unknown tool: " << name << std::endl;

    Tool* tool = it->second.get();

    const bool is_shell = ( name == "run_command" && args.contains("command"));
    std::string command = is_shell ? common::trim_ws(args["command"].to_string()) : "";
    std::string summary = is_shell ? command : (name + " " + args.dump_minified());

    std::string danger;
    if ( is_shell )
        danger = classify_danger(command);
    else if (( name == "write_file" || name == "edit_file" ) && args.contains("path"))
        danger = classify_path_danger(args["path"].to_string());

    // Program / key used for "allow session" and "allow similar".
    std::string similar_key = name;
    if ( is_shell ) {
        auto toks = tokenize(command);
        if ( !toks.empty())
            similar_key = basename_of(toks[0]);
    }
    std::string exact_key = is_shell ? command : summary;

    auto run = [&]() -> std::string {
        if ( _pre_run_cb )
            _pre_run_cb(name, args);
        if ( _activity_cb )
            _activity_cb(is_shell ? ("running: " + command) : ("running " + name));
        logger::info["tool"] << "executing " << name << std::endl;
        std::string result = tool->execute(args);
        if ( _activity_cb )
            _activity_cb("");
        return result;
    };

    // Insecure: no gating at all.
    if ( _mode == ConfirmMode::insecure )
        return run();

    const bool needs_confirm = tool->requires_confirmation() || !danger.empty();

    // Safe read-only tools (and, in automatic mode, ordinary tools) run freely —
    // but a danger-listed command always needs acknowledgement.
    if ( !needs_confirm )
        return run();
    if ( _mode == ConfirmMode::automatic && danger.empty())
        return run();

    // In confirm mode, a known read-only command runs without asking — unless
    // strict mode is on, or the command is danger-listed.
    if ( _mode == ConfirmMode::confirm && is_shell && danger.empty() &&
         !_strict && classify_safe(command))
        return run();

    // Previously granted this session?
    if ( _allow_exact.count(exact_key) || _allow_similar.count(similar_key))
        return run();

    // Fail safe: if confirmation is required but no UI is available to ask, deny.
    if ( !_confirm_cb ) {
        logger::warning["tool"] << "no confirmation UI; denying " << name << std::endl;
        return "tool call denied: " + name + " requires confirmation but none is available "
               "(use --insecure or --yes-tools for non-interactive runs)";
    }

    ConfirmRequest req;
    req.tool = name;
    req.summary = summary;
    req.danger = danger;
    req.similar_key = similar_key;
    req.can_similar = true;

    Decision d = _confirm_cb(req);
    switch ( d ) {
        case Decision::deny:
            logger::info["tool"] << "user declined " << name << std::endl;
            return "user declined to run " + name;
        case Decision::session:
            _allow_exact.insert(exact_key);
            break;
        case Decision::similar:
            _allow_similar.insert(similar_key);
            break;
        case Decision::once:
            break;
    }
    return run();
}

bool Registry::has(const std::string& name) const {
    return _tools.find(name) != _tools.end();
}

} // namespace agent::tools
