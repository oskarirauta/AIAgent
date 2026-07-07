#include "agent/tools/registry.hpp"

#include <sstream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <set>
#include <mutex>
#include <filesystem>
#include "throws.hpp"
#include "logger.hpp"
#include <cctype>
#include "common.hpp"
#include "agent/text_utils.hpp"
#include "agent/tools/read_file.hpp"
#include "agent/tools/write_file.hpp"
#include "agent/tools/edit_file.hpp"
#include "agent/tools/run_command.hpp"
#include "agent/tools/list_directory.hpp"
#include "agent/tools/grep.hpp"
#include "agent/tools/find_symbol.hpp"
#include "agent/tools/find_references.hpp"
#include "agent/tools/project_map.hpp"
#include "agent/tools/outline_file.hpp"

namespace agent::tools {

void Registry::register_defaults() {
    // Shared read/write stamp registry so a write can tell it would clobber a file
    // that changed on disk since the model read it.
    auto tracker = std::make_shared<FileTracker>();
    add(std::make_unique<ReadFile>(tracker));
    add(std::make_unique<WriteFile>(tracker));
    add(std::make_unique<EditFile>(tracker));
    add(std::make_unique<RunCommand>());
    add(std::make_unique<ListDirectory>());
    add(std::make_unique<Grep>());
    add(std::make_unique<FindSymbol>());
    add(std::make_unique<FindReferences>());
    add(std::make_unique<ProjectMap>());
    add(std::make_unique<OutlineFile>());
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

// Prefix every line of `text` with `mark` (for -/+ diff-style previews).
std::string mark_lines(const std::string& text, const char* mark, size_t cap = 40) {
    std::string out;
    std::istringstream is(text);
    std::string line;
    size_t n = 0;
    while ( std::getline(is, line)) {
        if ( n++ >= cap ) { out += std::string(mark) + " …\n"; break; }
        out += std::string(mark) + " " + line + "\n";
    }
    return out;
}

// Keep a preview from filling the confirm dialog: show at most `cap` lines and
// note how many were hidden (the full content is still written either way).
std::string cap_preview(const std::string& text, size_t cap = 60) {
    size_t nl = 0, cut = std::string::npos;
    for ( size_t i = 0; i < text.size(); ++i )
        if ( text[i] == '\n' && ++nl == cap ) { cut = i + 1; break; }
    if ( cut == std::string::npos )
        return text; // fewer than cap lines
    size_t more = static_cast<size_t>(std::count(text.begin() + cut, text.end(), '\n'));
    return text.substr(0, cut) + "… (" + std::to_string(more) + " more lines; full diff not shown)\n";
}

// A human-readable preview of what a write_file / edit_file call would change,
// shown in the confirmation dialog. Empty for other tools.
std::string change_preview(const std::string& name, const JSON& args) {
    if ( name == "write_file" && args.contains("path") && args.contains("content")) {
        std::string path = common::trim_ws(args["path"].to_string());
        std::string newc = args["content"].to_string();
        std::error_code ec;
        if ( !std::filesystem::exists(path, ec)) {
            size_t lines = static_cast<size_t>(std::count(newc.begin(), newc.end(), '\n')) + ( newc.empty() ? 0 : 1 );
            return "(new file, " + std::to_string(lines) + " lines)";
        }
        std::ifstream ifd(path, std::ios::binary);
        std::stringstream ss; ss << ifd.rdbuf();
        return cap_preview(agent::block_diff(ss.str(), newc, "current", "new"));
    }
    if ( name == "edit_file" && args.contains("path")) {
        auto one = [](const JSON& e) {
            std::string o = e.contains("old_string") ? e["old_string"].to_string() : "";
            std::string n = e.contains("new_string") ? e["new_string"].to_string() : "";
            return mark_lines(o, "-") + mark_lines(n, "+");
        };
        if ( args.contains("edits") && args["edits"] == JSON::TYPE::ARRAY ) {
            std::string out;
            const JSON& edits = args["edits"];
            for ( size_t i = 0; i < edits.size(); ++i )
                out += ( i ? "\n" : "" ) + one(edits[i]);
            return out;
        }
        return one(args);
    }
    return "";
}

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

// User-config extensions to the built-in lists (see set_extra_safe/_danger).
// Function-local statics avoid initialisation-order issues.
std::vector<std::string>& extra_safe_list() {
    static std::vector<std::string> v;
    return v;
}
std::vector<std::string>& extra_danger_list() {
    static std::vector<std::string> v;
    return v;
}

} // namespace

void Registry::set_extra_safe(const std::vector<std::string>& cmds) {
    extra_safe_list() = cmds;
}

void Registry::set_extra_danger(const std::vector<std::string>& cmds) {
    extra_danger_list() = cmds;
}

// Split a command line on the top-level sequencing / pipe / background connectors
// (&&, ||, ;, |, &, newline) so each stage is classified on its own. A dangerous
// command hidden after a benign prefix (`cd /tmp && rm -rf /`, `cd x; rm -rf y`)
// must never be judged by the prefix alone.
static std::vector<std::string> split_shell_segments(const std::string& command) {
    std::vector<std::string> parts;
    std::string cur;
    for ( size_t i = 0; i < command.size(); ) {
        if ( command[i] == '&' && i + 1 < command.size() && command[i + 1] == '&' ) {
            parts.push_back(cur); cur.clear(); i += 2;
        } else if ( command[i] == '|' && i + 1 < command.size() && command[i + 1] == '|' ) {
            parts.push_back(cur); cur.clear(); i += 2;
        } else if ( command[i] == ';' || command[i] == '|' || command[i] == '&' || command[i] == '\n' ) {
            parts.push_back(cur); cur.clear(); i += 1;
        } else {
            cur += command[i]; ++i;
        }
    }
    parts.push_back(cur);
    return parts;
}

// Danger classification for a SINGLE stage (no connectors). classify_danger below
// runs this over every stage of a compound command.
static std::string classify_danger_segment(const std::string& command) {
    std::vector<std::string> tokens = tokenize(command);
    if ( tokens.empty())
        return "";

    // Skip leading wrapper programs so `env FOO=bar rm -rf ~`, `timeout 5 rm -rf`,
    // `xargs rm`, `sudo rm` … are classified by the REAL command, not the wrapper
    // (which alone looks harmless). Best-effort: step over the wrapper and its own
    // leading args (VAR=val assignments, -flags, numeric durations).
    // Note: sudo/doas are deliberately NOT here — they are dangerous in their own
    // right (privilege escalation) and stay flagged by the rules below.
    static const std::set<std::string> wrappers = {
        "env", "nice", "ionice", "nohup", "time", "timeout", "stdbuf", "xargs", "setsid"
    };
    // An env assignment is `NAME=value` where NAME is an identifier — the value
    // may contain anything (incl. '/'), so `env PATH=/opt/bin rm -rf ~` must not
    // stop the skip at the assignment and mistake the value's basename for the
    // program, letting the real `rm -rf` slip past the danger rules.
    auto is_assign = [](const std::string& t) {
        size_t eq = t.find('=');
        if ( eq == 0 || eq == std::string::npos )
            return false;
        for ( size_t i = 0; i < eq; ++i ) {
            char ch = t[i];
            if ( !( std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' ))
                return false;
        }
        return true;
    };
    size_t first = 0;
    while ( first < tokens.size() && wrappers.count(basename_of(tokens[first]))) {
        ++first;
        while ( first < tokens.size()) {
            const std::string& t = tokens[first];
            bool flag = !t.empty() && t[0] == '-';
            // A numeric duration must contain a digit — otherwise program names
            // made only of [smhd] (sh, ssh) are wrongly skipped as durations.
            bool num = !t.empty() &&
                       t.find_first_of("0123456789") != std::string::npos &&
                       t.find_first_not_of("0123456789.smhd") == std::string::npos;
            if ( is_assign(t) || flag || num ) ++first; else break;
        }
    }
    if ( first >= tokens.size())
        first = 0;
    // Re-base so the danger-rule scan below sees the real program and its args.
    if ( first > 0 )
        tokens.erase(tokens.begin(), tokens.begin() + first);

    std::string program = basename_of(tokens[0]);

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

    // User-config danger additions (tools_danger) — checked on the REAL program,
    // i.e. after leading wrappers were stepped over above.
    for ( const auto& c : extra_danger_list())
        if ( program == basename_of(c))
            return "flagged as dangerous in your config (tools_danger)";

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

std::string Registry::classify_danger(const std::string& command) {
    // Whole-command checks that intentionally span the connectors.
    std::string lower = common::to_lower(command);
    if ( (lower.find("curl") != std::string::npos || lower.find("wget") != std::string::npos) &&
         (lower.find("| sh") != std::string::npos || lower.find("|sh") != std::string::npos ||
          lower.find("| bash") != std::string::npos || lower.find("|bash") != std::string::npos)) {
        return "pipes downloaded content straight into a shell";
    }
    if ( command.find(":(){") != std::string::npos )
        return "looks like a fork bomb";

    // Every stage is judged on its own, so a dangerous command after a benign
    // prefix (`cd /tmp && rm -rf /`) is still flagged.
    for ( const auto& seg : split_shell_segments(command)) {
        std::string r = classify_danger_segment(common::trim_ws(seg));
        if ( !r.empty())
            return r;
    }
    return "";
}

// The command that "allow all commands like this" should be keyed on: the first
// real program, stepping over a leading `cd <dir>` (and its connector) plus the
// wrapper programs, so a benign `cd` prefix can never make a whole compound get
// blanket-allowed under the key "cd".
static std::string primary_program(const std::string& command) {
    for ( const auto& seg : split_shell_segments(command)) {
        std::string s = common::trim_ws(seg);
        if ( s.empty())
            continue;
        std::vector<std::string> toks = tokenize(s);
        if ( toks.empty())
            continue;
        std::string prog = basename_of(toks[0]);
        if ( prog == "cd" || prog == "pushd" || prog == "popd" )
            continue; // keep looking for the command that actually does something
        return prog;
    }
    // All stages were cd/empty — key on cd then (it really is just a cd).
    std::vector<std::string> toks = tokenize(common::trim_ws(command));
    return toks.empty() ? command : basename_of(toks[0]);
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

    // User-config additions (tools_safe / tools_danger). Danger wins over safe.
    for ( const auto& c : extra_danger_list())
        if ( prog == basename_of(c))
            return false;
    for ( const auto& c : extra_safe_list())
        if ( prog == basename_of(c))
            return true;

    // Read-only commands with no side effects.
    static const std::vector<std::string> safe = {
        "date", "cal", "pwd", "whoami", "hostname", "id", "uname", "which",
        "ls", "cat", "head", "tail", "df", "free", "echo", "printenv",
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

    // Plan mode: read-only tools work; anything that writes or runs is refused
    // so the model produces a plan instead of acting.
    if ( _plan_mode && tool->mutates())
        return "plan mode is on — `" + name + "` is disabled (no file writes or "
               "commands). Present your plan for the user to review; they will turn "
               "plan mode off (/plan off) before you make any changes.";

    const bool is_shell = ( name == "run_command" && args.contains("command"));
    std::string command = is_shell ? common::trim_ws(args["command"].to_string()) : "";
    std::string summary;
    if ( is_shell )
        summary = command;
    else if (( name == "write_file" || name == "edit_file" ) && args.contains("path"))
        summary = name + " " + common::trim_ws(args["path"].to_string()); // the diff is the preview
    else
        summary = name + " " + args.dump_minified();

    std::string danger;
    if ( is_shell )
        danger = classify_danger(command);
    else if (( name == "write_file" || name == "edit_file" ) && args.contains("path"))
        danger = classify_path_danger(args["path"].to_string());
    if ( danger.empty())
        danger = tool->danger_reason(args); // e.g. an MCP destructiveHint

    // Program / key used for "allow session" and "allow similar". For a shell
    // command this is the first REAL program (past a leading `cd`), so allowing
    // "all commands like this" can never blanket-approve a compound under "cd".
    std::string similar_key = name;
    if ( is_shell )
        similar_key = primary_program(command);
    std::string exact_key = is_shell ? command : summary;

    auto run = [&]() -> std::string {
        if ( _pre_run_cb )
            _pre_run_cb(name, args);
        if ( _activity_cb )
            _activity_cb(is_shell ? ("running: " + command) : ("running " + name));
        {
            // execute() runs concurrently for read-only tool batches, and the
            // logger is not thread-safe — interleaved writes corrupt its state.
            static std::mutex log_mx;
            std::lock_guard<std::mutex> lk(log_mx);
            logger::info["tool"] << "executing " << name << std::endl;
        }
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

    // Granted for the rest of this turn (danger-listed calls still ask)?
    if ( _turn_grant && danger.empty())
        return run();

    // Previously granted this session? A danger-flagged call never rides a broad
    // "allow similar" grant — even if the program was allow-similar'd on a benign
    // invocation, a dangerous one (rm -rf /, mkfs, …) still asks.
    if ( _allow_exact.count(exact_key)) { ++_grant_uses[exact_key]; return run(); }
    if ( danger.empty() && _allow_similar.count(similar_key)) { ++_grant_uses[similar_key]; return run(); }

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
    req.preview = change_preview(name, args);
    // Don't offer "allow all like this" for a dangerous command — that is exactly
    // how a blanket grant would later wave through a compound's dangerous stage.
    req.can_similar = danger.empty();

    std::string note;
    Decision d = _confirm_cb(req, note);
    switch ( d ) {
        case Decision::deny:
            logger::info["tool"] << "user declined " << name << std::endl;
            return note.empty()
                ? "user declined to run " + name
                : "user declined to run " + name + " — " + note;
        case Decision::turn:
            _turn_grant = true;
            break;
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

bool Registry::ask_continue(const std::string& summary) {
    if ( _mode == ConfirmMode::insecure )
        return true; // insecure: never interrupt
    if ( !_confirm_cb )
        return false; // non-interactive: stop safely
    ConfirmRequest req;
    req.tool = "continue the turn";
    req.summary = summary;
    req.can_similar = false;
    std::string note;
    return _confirm_cb(req, note) != Decision::deny;
}

std::vector<Registry::Grant> Registry::grants() const {
    std::vector<Grant> g;
    for ( const auto& k : _allow_exact ) {
        auto it = _grant_uses.find(k);
        g.push_back({ "session", k, it == _grant_uses.end() ? 0 : it->second });
    }
    for ( const auto& k : _allow_similar ) {
        auto it = _grant_uses.find(k);
        g.push_back({ "similar", k, it == _grant_uses.end() ? 0 : it->second });
    }
    return g;
}

size_t Registry::revoke_grant(const std::string& key) {
    size_t n = _allow_exact.erase(key) + _allow_similar.erase(key);
    _grant_uses.erase(key);
    return n;
}

size_t Registry::revoke_all_grants() {
    size_t n = _allow_exact.size() + _allow_similar.size();
    _allow_exact.clear();
    _allow_similar.clear();
    _grant_uses.clear();
    _turn_grant = false;
    return n;
}

} // namespace agent::tools
