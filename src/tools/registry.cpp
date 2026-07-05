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
#include "agent/tools/run_command.hpp"
#include "agent/tools/list_directory.hpp"
#include "agent/tools/grep.hpp"

namespace agent::tools {

void Registry::register_defaults() {
    add(std::make_unique<ReadFile>());
    add(std::make_unique<WriteFile>());
    add(std::make_unique<RunCommand>());
    add(std::make_unique<ListDirectory>());
    add(std::make_unique<Grep>());
}

void Registry::add(std::unique_ptr<Tool> tool) {
    _tools[tool->name()] = std::move(tool);
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

    for ( const auto& rule : danger_rules()) {
        if ( rule.program != program )
            continue;
        if ( rule.any_flags.empty())
            return rule.reason;
        for ( const auto& flag : rule.any_flags ) {
            for ( size_t i = 1; i < tokens.size(); ++i ) {
                if ( tokens[i] == flag || tokens[i].find(flag) != std::string::npos )
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
    else if ( name == "write_file" && args.contains("path"))
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
