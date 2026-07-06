#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agent/syntax_highlighter.hpp"
#include "agent/tools/registry.hpp"
#include "agent/token_stats.hpp"
#include "agent/theme.hpp"

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
    tools::Decision confirm(const tools::ConfirmRequest& req);

    // Publish what the worker is currently doing (e.g. a running command) so the
    // status line can show it. Thread-safe.
    void set_activity(const std::string& activity);

    // Handler for slash commands (other than /exit and /quit), run locally on the
    // main thread; returns text to show as a system message.
    void set_command_callback(command_cb_t cb) { _command_cb = std::move(cb); }

    // Restore the terminal from raw mode. Safe to call more than once.
    void teardown();

    // Last-resort restore from the signal handler before a forced exit.
    static void emergency_teardown();

private:
    // Terminal / raw mode.
    void setup();
    int  term_cols() const;

    // Live block (input + status) at the bottom of the screen.
    void draw_live();
    void erase_live();
    std::string status_line() const;

    // Transcript output.
    void echo_user(const std::string& display);
    void begin_reply();
    void emit_styled_line(const std::string& line); // one committed, styled line
    std::string style_spans(const std::string& line, Language lang) const;

    // Turn lifecycle (worker thread + main-thread event loop).
    void on_enter();
    void start_turn(const std::string& line, const std::string& display);
    void start_async_command(const std::string& cmd, const std::string& activity,
                             const std::string& echo_label = ""); // run a slow command off-thread
    bool maybe_auto_compact(); // auto-summarise history when it nears the context budget; true if started
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

    // Interactive settings menu (arrow-select), opened by a bare /settings.
    struct SettingRow {
        std::string key;                  // command key (theme, tools, model, …)
        std::string label;                // display label
        std::string value;                // current value
        std::vector<std::string> options; // cyclable values; empty = free text
    };
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
    Config& _config;
    const Conversation& _conversation;
    const TokenStats& _stats;

    std::string _input;                 // current input buffer (UTF-8, may hold placeholders)
    size_t _cursor = 0;                 // byte offset of the cursor within _input
    size_t _input_window_start = 0;     // horizontal scroll offset (display cells) for the prompt
    bool _esc_pending = false;          // a lone ESC seen; the next key is its (possibly delayed) follow-up
    std::vector<PasteItem> _pastes;     // large pastes referenced by inline placeholders
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

    // Main-thread turn state.
    bool _turn_running = false;
    bool _async_command = false;        // the worker is running a slow slash command, not a turn
    std::string _async_cmd_line;        // the command line, echoed with its result on completion
    bool _confirming = false;
    int _confirm_selection = 0;   // highlighted option (0 = Deny, the safe default)
    int _confirm_menu_lines = 0;  // option lines currently drawn (for in-place redraw)

    bool _in_settings = false;
    bool _settings_editing = false;    // typing a free-text value into the selected row
    std::string _settings_edit_buf;
    int _settings_selection = 0;
    int _settings_menu_lines = 0;
    std::vector<SettingRow> _settings_rows;
    std::queue<std::string> _pending;      // prompts queued while a turn is running
    int _spin = 0;
    std::chrono::steady_clock::time_point _turn_start;
};

} // namespace agent
