#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include "agent/tools/tool.hpp"

namespace agent::tools {

// Confirmation policy for tool calls.
//   confirm   - ask before every confirmation-requiring tool (default)
//   automatic - run ordinary tools without asking, but still warn on danger-listed ones
//   insecure  - run everything without asking
enum class ConfirmMode { confirm, automatic, insecure };

// Outcome of a confirmation prompt.
enum class Decision {
    deny,     // do not run
    once,     // run this one time
    session,  // run, and don't ask again for this exact command this session
    similar   // run, and don't ask again for similar commands (same program) this session
};

// Everything the UI needs to render a confirmation prompt.
struct ConfirmRequest {
    std::string tool;        // tool name
    std::string summary;     // full human-readable action / command
    std::string danger;      // reason string if the command is danger-listed, else empty
    std::string similar_key; // what "allow similar" would whitelist (e.g. program name)
    std::string preview;     // optional diff/preview of the change (write_file/edit_file)
    bool can_similar = false;
};

class Registry {
public:
    using confirm_cb_t = std::function<Decision(const ConfirmRequest&)>;
    using activity_cb_t = std::function<void(const std::string&)>;
    // Invoked just before a tool actually runs (after any confirmation), so the
    // app can observe it — e.g. snapshot a file before write_file overwrites it.
    using pre_run_cb_t = std::function<void(const std::string& name, const JSON& args)>;

    void register_defaults();
    void add(std::unique_ptr<Tool> tool);
    void remove(const std::string& name); // drop a registered tool (e.g. the advisor)
    void set_confirm_callback(confirm_cb_t cb);
    void set_activity_callback(activity_cb_t cb) { _activity_cb = std::move(cb); }
    void set_pre_run_callback(pre_run_cb_t cb) { _pre_run_cb = std::move(cb); }
    void set_mode(ConfirmMode mode) { _mode = mode; }
    void set_strict(bool strict) { _strict = strict; } // ignore the safe-command list when true

    JSON schema() const;
    std::string execute(const std::string& name, const JSON& args);
    bool has(const std::string& name) const;

    // Classify a shell command against the danger list. Returns a human-readable
    // reason when the command is risky, or an empty string otherwise. Exposed for
    // testing.
    static std::string classify_danger(const std::string& command);

    // Config-extensible command lists, set once at startup from the USER's config
    // file (never from project files, which the model itself can write): extra
    // read-only commands that skip confirmation, and extra programs that always
    // warn. A program on both lists is treated as dangerous.
    static void set_extra_safe(const std::vector<std::string>& cmds);
    static void set_extra_danger(const std::vector<std::string>& cmds);

    // Classify a file-write target path: writing into a system directory or
    // outside the working directory is risky. Returns a reason or empty string.
    static std::string classify_path_danger(const std::string& path);

    // Whether a shell command is a known read-only / side-effect-free command
    // (date, ls, pwd, git status, gcc -v, …) that can run without confirmation
    // in confirm mode (unless strict mode is on). Exposed for testing.
    static bool classify_safe(const std::string& command);

private:
    std::map<std::string, std::unique_ptr<Tool>> _tools;
    confirm_cb_t _confirm_cb;
    activity_cb_t _activity_cb;
    pre_run_cb_t _pre_run_cb;
    ConfirmMode _mode = ConfirmMode::confirm;
    bool _strict = false;

    // Session-scoped approvals granted via "allow session" / "allow similar".
    std::set<std::string> _allow_exact;   // full command / action strings
    std::set<std::string> _allow_similar; // program names / tool names
};

} // namespace agent::tools
