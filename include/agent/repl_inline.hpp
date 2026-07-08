#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <deque>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agent/syntax_highlighter.hpp"
#include "agent/tools/registry.hpp"
#include "agent/token_stats.hpp"
#include "agent/theme.hpp"
#include "agent/workflow.hpp"

namespace agent {

class Config;
class Conversation;

// Inline REPL renderer.
//
// Unlike the old full-screen ncurses UI, this never switches to the terminal's
// alternate screen. The transcript is printed as ordinary stdout lines that flow
// into the terminal's native scrollback, so mouse-wheel scrolling and drag-to-
// copy work across the whole history. Only a small "live block" at the bottom
// (the input line plus a status line) is managed with ANSI escape sequences.
class InlineRepl {
public:
    using callback_t = std::function<std::string(const std::string&, std::function<void(const std::string&)>, std::atomic<bool>*)>;
    using command_cb_t = std::function<std::string(const std::string&)>; // slash command -> result text

    struct PasteItem {
        std::string placeholder; // token shown inline in the input
        std::string content;     // full pasted text substituted on submit
    };

    InlineRepl(callback_t cb, Config& config, const Conversation& conversation, const TokenStats& stats);
    ~InlineRepl();

    void run();

    // Tool-confirmation request from the worker thread. Blocks the worker until
    // the main (UI) thread renders the prompt and reads the user's choice.
    tools::Decision confirm(const tools::ConfirmRequest& req, std::string& note);

    // Synchronous confirm on the MAIN thread (renders and reads keys itself) —
    // for a startup prompt (e.g. approving project MCP) run from set_on_ready,
    // before the event loop exists to service the worker-thread confirm() above.
    tools::Decision confirm_on_main(const tools::ConfirmRequest& req, std::string& note);

    // ask_user tool: the worker asks the user a question and blocks until the
    // main thread presents it (a menu for options, else a text prompt) and reads
    // the answer. Returns the user's answer (empty if cancelled).
    std::string ask_user(const std::string& question, const std::vector<std::string>& options);

    // Publish what the worker is currently doing (e.g. a running command) so the
    // status line can show it. Thread-safe.
    void set_activity(const std::string& activity);

    // Queue a one-line notice (e.g. "workflow #2 done") to be printed above the
    // live block by the main loop, with a terminal bell. Thread-safe; callable
    // from background threads.
    void notify(const std::string& line);

    // Same, but rendered dim and without the bell — for routine progress notes
    // (one compact line per executed tool call).
    void notify_quiet(const std::string& line);

    // A tool-execution notice: notify_quiet plus a per-turn counter for the
    // end-of-turn digest. Thread-safe (parallel tool batches).
    void notify_tool(const std::string& line);

    // Queue a synthetic prompt (workflow auto-resume). Thread-safe; it joins the
    // SAME pending queue as user messages, so it runs through the normal turn
    // machinery when the REPL is idle. Bounded: at most 2 auto prompts are
    // accepted per real user message — returns false when dropped by the cap.
    bool enqueue_prompt(const std::string& text);

    // Handler for slash commands (other than /exit and /quit), run locally on the
    // main thread; returns text to show as a system message.
    void set_command_callback(command_cb_t cb) { _command_cb = std::move(cb); }
    void set_workflows_provider(std::function<std::vector<WorkflowRun>()> fn) { _wf_provider = std::move(fn); }
    // Multi-level workflows drill-down: runs → steps → one step's content. Public so
    // the navigation can be driven directly (dispatch + tests); run_id >= 0 opens
    // straight into that run's steps.
    void open_workflows_menu(int run_id = -1);
    void workflow_enter(); // runs → steps → content
    void workflow_up();    // content → steps → runs → close
    bool in_list_menu() const { return _in_list; }
    // Run once after the terminal is in raw mode but before the input loop — the
    // right moment for a startup confirm (e.g. approving project-local MCP).
    void set_on_ready(std::function<void()> cb) { _on_ready = std::move(cb); }

    // Restore the terminal from raw mode. Safe to call more than once.
    void teardown();

    // Last-resort restore from the signal handler before a forced exit.
    static void emergency_teardown();

private:
    // Terminal / raw mode.
    void setup();
    int  term_cols() const;
    int  term_rows() const;

    // ── Shared scrollable list/reader menu ───────────────────────────────
    // Used by /workflows, /history, /memories, /tasks, /skills, /queue: a
    // dismissable, scrolling list instead of dumping a long block into the
    // transcript. Enter drills into a selected row (runs `drill_cmd + key`), and
    // an optional action key runs `action_cmd + key` (e.g. /queue drop <n>).
    struct ListMenu {
        std::string title;
        std::vector<std::string> rows;   // one display line per entry
        std::vector<std::string> keys;   // per-row drill/action key (parallel; may be empty)
        std::string drill_cmd;           // Enter runs this + keys[sel] and shows the result
        std::vector<std::string> details; // pre-loaded per-row detail (Enter shows details[sel])
        std::string select_cmd;          // (picker) Enter runs this + keys[sel] and CLOSES
        std::string current;             // pre-select the row whose key equals this
        struct Action { char key; std::string cmd; std::string label; };
        std::vector<Action> actions;     // each: `key` runs `cmd + keys[sel]`
        std::string reopen_cmd;          // re-run to refresh the list after an action
        std::string hint;                // overrides the computed footer hint when set
    };
    void open_list_menu(ListMenu menu);
    void open_list_detail(const std::string& title, const std::string& text);
    void draw_list_menu(bool redraw);
    int menu_view_rows() const; // visible rows in a list/reader panel (capped so the transcript stays visible above)

    void build_workflow_level(bool redraw);    // (re)build _list for the current level from a live snapshot
    void handle_workflow_key(int c);
    void handle_list_key(int c);
    void close_list_menu();

    // Live block (input + status) at the bottom of the screen.
    void draw_live();
    void erase_live();
    std::string status_line() const;

    // Transcript output.
    void echo_user(const std::string& display);
    // Echo several queued user messages as one group — each with its own "›"
    // marker and no blank line between, so a flushed backlog reads as several
    // user messages, not alternating speakers.
    void echo_user_multi(const std::vector<std::string>& parts);
    void begin_reply();
    void emit_styled_line(const std::string& line); // one committed, styled line
    std::string style_spans(const std::string& line, Language lang) const;

    // Turn lifecycle (worker thread + main-thread event loop).
    void on_enter();
    void start_turn(const std::string& line, const std::string& display, bool already_echoed = false);
    void start_async_command(const std::string& cmd, const std::string& activity,
                             const std::string& echo_label = ""); // run a slow command off-thread
    bool maybe_auto_compact(); // auto-summarise history when it nears the context budget; true if started
    std::string budget_warning(); // one-shot warning text when the session nears its cost/token budget
    void poll_worker();
    void finish_turn();
    void finish_async_command();
    void flush_lines();  // emit complete lines from _line_buf (block already erased)
    void emit_reply_line(const std::string& line); // one reply line, trimming blank edges
    void route_stream_chunk(const std::string& chunk); // split think preview vs answer in collapse mode
    std::vector<std::string> think_preview_lines(int cols) const; // transient collapse-mode preview
    void render_command(const std::string& cmd, const std::string& result);
    void render_confirm_dialog(const tools::ConfirmRequest& req);
    void draw_confirm_menu(const tools::ConfirmRequest& req, bool redraw);
    void handle_confirm_key(int c);
    void commit_confirm(tools::Decision d, const std::string& label);

    void render_ask_dialog();
    void draw_ask_menu(bool redraw);
    void handle_ask_key(int c);
    void commit_ask(const std::string& answer);

    // Interactive settings menu (arrow-select), opened by a bare /settings.
    struct SettingRow {
        std::string key;                  // command key (theme, tools, model, …)
        std::string label;                // display label
        std::string value;                // current value
        std::vector<std::string> options; // cyclable enum values; empty = free text / number
        std::string group;                // section header this row sits under
        std::string desc;                 // one-line help shown under the selected row
        std::string unit;                 // suffix for a numeric value ("tokens", "lines")
        bool is_number = false;           // adjustable with ←/→ (and editable with Enter)
        long num_min = 0, num_max = 0, num_step = 1;
        std::string zero_label;           // shown instead of "0" (e.g. "all", "unlimited")
    };
    std::string setting_display_value(const SettingRow& row) const; // value as shown
    void adjust_number_row(int dir);      // ←/→ on a numeric row
    void run_command_line(const std::string& trimmed); // execute a slash command (called when idle)
    void drain_pending();                               // run queued commands/messages after a turn
    void render_context();                              // visual /context usage breakdown
    void open_settings_menu();
    void draw_settings_menu(bool redraw);
    void handle_settings_key(int c);
    void close_settings_menu();
    void cycle_settings_row(int dir); // dir = +1 / -1
    void apply_settings_edit();       // commit the free-text edit buffer

    // Multi-line prompt helpers: byte ranges of each visual line of _input when
    // wrapped to `width` columns (split on '\n', hard-wrapped by display width).
    std::vector<std::pair<size_t, size_t>> wrap_input(int width) const;
    bool multiline_vertical(int dir); // move the cursor up/down a visual line; false at the edge

    // Input handling (raw mode line editor).
    void handle_byte(int c);
    void insert_text(const std::string& text);
    void backspace();
    void handle_tab();          // autocomplete slash commands / file paths
    std::string highlight_keywords(const std::string& body) const; // colour ultracode/ultrathink
    void delete_word_before();  // Ctrl-W
    void kill_to_line_start();  // Ctrl-U
    void kill_to_line_end();    // Ctrl-K
    void prune_pastes();        // forget placeholders no longer present in _input
    void move_left();
    void move_right();
    void history_prev();
    void history_next();
    void read_bracketed_paste();
    std::string expand_input() const; // input with paste placeholders substituted
    std::string substitute_pastes(std::string s) const; // placeholder -> content

    // UTF-8 helpers operating on byte indices within _input.
    size_t prev_char(size_t pos) const;
    size_t next_char(size_t pos) const;
    int    display_width(const std::string& s) const;

    // Paste placeholders are atomic: the cursor jumps over them and backspace
    // removes the whole box. These return the [begin,end) byte range of the
    // placeholder bordering `pos`, or {npos,npos} when there is none.
    std::pair<size_t, size_t> placeholder_ending_at(size_t pos) const;
    std::pair<size_t, size_t> placeholder_starting_at(size_t pos) const;
    void drop_paste(const std::string& placeholder); // forget a removed paste

    callback_t _callback;
    command_cb_t _command_cb;
    std::function<void()> _on_ready;
    Config& _config;
    const Conversation& _conversation;
    const TokenStats& _stats;

    std::string _input;                 // current input buffer (UTF-8, may hold placeholders)
    size_t _cursor = 0;                 // byte offset of the cursor within _input
    size_t _input_window_start = 0;     // horizontal scroll offset (display cells) for the prompt
    bool _esc_pending = false;          // a lone ESC seen; the next key is its (possibly delayed) follow-up
    std::vector<PasteItem> _pastes;     // large pastes referenced by inline placeholders
    std::vector<PasteItem> _sent_pastes; // pastes from sent messages, for /paste <n>
    size_t _paste_counter = 0;

    std::vector<std::string> _prompt_history; // previous user inputs
    size_t _history_index = 0;          // == size() means "current (unsubmitted) line"
    std::string _stashed_input;         // current line stashed while browsing history

    int _live_lines = 0;                // physical lines the live block currently occupies
    int _live_cursor_up = 2;            // lines from the cursor up to the block top (for erase/redraw)
    bool _in_reply = false;             // between begin_reply()/end_reply()

    // Streaming line buffer + fenced-code state for syntax highlighting.
    std::string _line_buf;
    bool _in_code = false;
    Language _code_lang = Language::none;

    // Blank-line trimming for the reply: skip leading blank lines, defer interior
    // ones (flushed only when more content follows), drop trailing ones.
    int  _pending_blanks = 0;
    bool _reply_has_content = false;
    bool _reply_first_line = false; // the reply's first printed line gets the AI marker
    bool _reply_dim = false;        // inside a streamed "thinking" region (rendered dim, 💭 marker)

    // Collapse mode: reasoning is shown live in a transient preview inside the live
    // block (never committed to the transcript), then vanishes once the answer
    // arrives or the turn ends. _stream_in_think tracks the \x01…\x02 region.
    std::string _think_preview;
    bool _stream_in_think = false;

    SyntaxHighlighter _highlighter{1};
    Theme _theme = theme_dark();
    bool _raw_active = false;

    // Handle the UI-local /theme command (returns text to display).
    std::string apply_theme_command(const std::string& line);

    // ── concurrency ──────────────────────────────────────────────────────
    // The LLM turn runs on a worker thread; the main thread keeps reading the
    // keyboard and repainting the pinned input block, so the user can type (and
    // queue) while the AI works, and Ctrl-C can abort a hung request.
    std::thread _worker;
    mutable std::mutex _mx;
    std::condition_variable _cv;
    std::queue<std::string> _out_chunks;   // streamed text from the worker (guarded)
    bool _turn_done = false;               // guarded
    std::string _turn_reply;               // guarded
    bool _turn_streamed = false;           // guarded
    std::string _activity;                 // current worker activity (guarded)

    // Tool-confirmation handshake (guarded by _mx / _cv).
    bool _confirm_pending = false;
    tools::ConfirmRequest _confirm_req;
    bool _confirm_answered = false;
    tools::Decision _confirm_decision = tools::Decision::deny;
    std::string _confirm_note;          // one-line reason captured with a "Deny with a reason"

    // ask_user handshake (worker asks a question, main thread answers) — mirrors
    // the confirm handshake, guarded by _mx / _cv.
    bool _ask_pending = false;
    std::string _ask_question;
    std::vector<std::string> _ask_options;
    bool _ask_answered = false;
    std::string _ask_answer;
    bool _asking = false;               // the ask dialog is on screen (main thread)
    int _ask_menu_lines = 0;            // lines drawn (for in-place redraw)
    int _ask_sel = 0;                   // selected option
    std::string _ask_input;             // free-text answer being typed (no options)

    // Main-thread turn state.
    bool _turn_running = false;
    bool _async_command = false;        // the worker is running a slow slash command, not a turn
    std::string _async_cmd_line;        // the command line, echoed with its result on completion
    bool _confirming = false;
    int _confirm_selection = 0;   // highlighted option (0 = Deny, the safe default)
    int _confirm_menu_lines = 0;  // option lines currently drawn (for in-place redraw)
    bool _confirm_note_mode = false; // typing a deny reason instead of picking an option
    std::string _confirm_note_buf;   // the reason being typed

    bool _in_list = false;             // a scrollable list/reader menu is open
    ListMenu _list;
    int _list_sel = 0;                 // selected row
    int _list_top = 0;                 // first visible row (scroll offset)
    int _list_lines = 0;              // lines the menu occupies (for redraw backup)
    bool _list_detail = false;         // showing a drilled-in row's detail
    std::vector<std::string> _list_detail_rows;
    int _list_detail_top = 0;
    // Workflows drill-down state (the list menu is in workflow mode when _wf_active).
    std::function<std::vector<WorkflowRun>()> _wf_provider;
    bool _wf_active = false;            // the open list menu is the workflow drill-down
    int _wf_level = 0;                  // 0 = runs, 1 = steps (content uses _list_detail)
    int _wf_run_id = -1;               // the run whose steps are shown at level 1

    bool _in_settings = false;
    bool _settings_editing = false;    // typing a free-text value into the selected row
    std::string _settings_edit_buf;
    int _settings_selection = 0;
    int _settings_menu_lines = 0;
    std::vector<SettingRow> _settings_rows;
    std::deque<std::string> _pending;      // prompts queued while a turn is running (guarded by _mx)
    void queue_command(const std::string& line); // /queue [drop <n|all>] — inspect/edit _pending
    int _auto_since_user = 0;              // auto-resume chain guard (guarded by _mx)
    struct Notice { std::string text; bool bell; };
    std::queue<Notice> _notices;           // async notices (guarded by _mx)
    void drain_notices();                  // print queued notices above the live block

    // While true, draw_live() is a no-op: the main loop is feeding a burst of
    // buffered input (an unbracketed paste) and will redraw once at the end —
    // otherwise every pasted byte re-wraps and redraws the whole input block.
    bool _defer_draw = false;
    int _spin = 0;
    int _budget_notified = 0; // highest budget threshold already warned (0 / 80 / 100)
    std::chrono::steady_clock::time_point _turn_start;
    std::atomic<int> _turn_tool_count{ 0 }; // tools run this turn (for the digest)
    bool _confirm_belled = false;           // a blocking-confirm bell already rang this turn
};

} // namespace agent
