#include "agent/repl_inline.hpp"
#include <set>

#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/statvfs.h>
#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <sstream>
#include <algorithm>
#include <map>
#include <cctype>
#include <utility>
#include <vector>

#include "agent/config.hpp"
#include "agent/conversation.hpp"
#include "agent/memory.hpp"
#include "agent/signal_handler.hpp"
#include "agent/text_utils.hpp"
#include "common.hpp"
#include "logger.hpp"

namespace agent {

// ── terminal state, shared with the signal handler's emergency restore ──
static struct termios g_orig_termios;
static bool g_termios_saved = false;

// Terminal-bell policy as an ordered level:
//   never < ask_user < question < attention < always.
// An event rings if the configured level is at least the event's threshold:
//   ask_user tool = 1, answer-is-a-question = 2, tool-permission/workflow = 3,
//   every answer = 4.
static int bell_level(const std::string& mode) {
    if ( mode == "never" ) return 0;
    if ( mode == "ask_user" || mode == "ask" ) return 1;
    if ( mode == "question" ) return 2;
    if ( mode == "always" ) return 4;
    return 3; // attention (default)
}

// Local/display-only slash commands: safe to run mid-turn because they only read
// state or change the UI — they never touch the running conversation, the model,
// or the provider. Anything not listed here queues while a turn is streaming.
static bool command_runs_immediately(const std::string& trimmed) {
    std::string cmd = trimmed;
    size_t sp = cmd.find_first_of(" \t");
    if ( sp != std::string::npos ) cmd = cmd.substr(0, sp);
    static const std::set<std::string> immediate = {
        // read-only displays / menus
        "/about", "/info", "/status", "/stats", "/diagnose", "/stop", "/interrupt", "/help", "/theme", "/settings", "/workflows",
        "/trust", "/history", "/memories", "/roadmap", "/tasks", "/skills", "/pins",
        "/context", "/cost", "/changes", "/mcp", "/paste", "/raw", "/limits", "/jobs",
        "/sessions",
        // /shell jumps the queue by design, but never RUNS mid-turn: a full
        // terminal handover cannot share the tty with a streaming reply, so
        // run_command_line only prints a "try again when idle" notice then.
        "/shell",
        // settings that only affect the NEXT request — running them mid-turn just
        // updates local state (last value wins: /effort medium then /effort max
        // leaves only max, a natural dedup), applied before the next prompt. They
        // don't change the running turn's tool gating or rebuild the conversation.
        "/effort", "/thinking", "/stream", "/model", "/autoresume", "/bell"
    };
    return immediate.count(cmd) > 0;
}

static std::string pending_kind_label(InlineRepl::PendingKind kind) {
    switch ( kind ) {
    case InlineRepl::PendingKind::LiveNote: return "steer/btw";
    case InlineRepl::PendingKind::Shell: return "shell";
    case InlineRepl::PendingKind::Command: return "command";
    default: return "message";
    }
}

static std::string queue_preview(std::string text, size_t limit) {
    for ( char& ch : text )
        if ( ch == '\n' || ch == '\r' || ch == '\t' )
            ch = ' ';
    text = common::trim_ws(text);
    if ( text.size() > limit )
        text = text.substr(0, limit) + "…";
    return text;
}

static void wr(const std::string& s) {
    if ( !s.empty())
        (void)::write(STDOUT_FILENO, s.data(), s.size());
}

static int read_byte() {
    unsigned char b;
    ssize_t n = ::read(STDIN_FILENO, &b, 1);
    if ( n == 1 )
        return b;
    return -1; // EOF or EINTR
}

// Split a UTF-8 string into display cells, one per codepoint (combining/wide
// characters are treated as width 1 — good enough for the prompt line).
static std::vector<std::string> split_cells(const std::string& s) {
    std::vector<std::string> cells;
    for ( size_t i = 0; i < s.size(); ) {
        size_t j = i + 1;
        while ( j < s.size() && (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80 )
            ++j;
        cells.push_back(s.substr(i, j - i));
        i = j;
    }
    return cells;
}

// Greedy word-wrap a prose line to `width` display columns, preserving leading
// indentation on every wrapped segment. A single over-long word (URL, token) is
// left to overflow rather than broken. Lines that already fit are returned as-is.
static std::vector<std::string> word_wrap(const std::string& line, int width) {
    if ( width < 8 )
        width = 8;
    if ( static_cast<int>(split_cells(line).size()) <= width )
        return { line };

    size_t ie = 0;
    while ( ie < line.size() && ( line[ie] == ' ' || line[ie] == '\t' ))
        ++ie;
    std::string indent = line.substr(0, ie);
    int indent_w = static_cast<int>(split_cells(indent).size());

    std::vector<std::string> out;
    std::string cur = indent;
    int cur_w = indent_w;

    std::istringstream iss(line.substr(ie));
    std::string word;
    while ( iss >> word ) {
        int ww = static_cast<int>(split_cells(word).size());
        bool has_word = cur_w > indent_w;
        if ( has_word && cur_w + 1 + ww > width ) {
            out.push_back(cur);
            cur = indent;
            cur_w = indent_w;
            has_word = false;
        }
        if ( has_word ) { cur += " "; cur_w += 1; }
        cur += word;
        cur_w += ww;
    }
    out.push_back(cur);
    return out;
}

static std::string thinking_style_for_theme(const Theme& theme) {
    // Reasoning should be quieter than the final answer, but more legible than
    // generic dim chrome/tool notices. Pick a slightly lighter neutral tone for
    // the built-ins; custom themes can still override `dim`/roles normally.
    if ( theme.name == "light" ) return "\033[38;5;240m";
    if ( theme.name == "cool" )  return "\033[38;5;110m";
    if ( theme.name == "rose" )  return "\033[38;5;146m";
    return "\033[38;5;248m";
}

// ── construction / teardown ─────────────────────────────────────────────

InlineRepl::InlineRepl(callback_t cb, Config& config, const Conversation& conversation, const TokenStats& stats)
    : _callback(std::move(cb)), _config(config), _conversation(conversation), _stats(stats) {
    _theme = build_theme(config.theme);
}

Theme InlineRepl::build_theme(const std::string& name) const {
    // "custom" is the config's base palette with its per-role overrides applied;
    // everything else is a built-in palette.
    if ( name == "custom" )
        return theme_custom(_config.theme_base, _config.theme_colors);
    return theme_by_name(name);
}

std::string InlineRepl::apply_theme_command(const std::string& line) {
    std::string arg;
    {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;       // "/theme"
        iss >> arg;       // name
    }
    // "custom" is offered only once the config actually defines overrides —
    // otherwise it would just be a second name for the base palette.
    bool has_custom = !_config.theme_colors.empty();
    std::string list = "dark, light, warm, cool, rose";
    if ( has_custom )
        list += ", custom";

    if ( arg.empty()) {
        std::string s = "theme: " + _theme.name + "  (available: " + list + ")";
        if ( _theme.name == "custom" )
            s += "\n  custom = base " + _config.theme_base + " with " +
                 std::to_string(_config.theme_colors.size()) + " override(s): " + theme_overrides_summary();
        else if ( !has_custom )
            s += "\n  a custom palette: put `theme_base: " + _theme.name + "` and e.g. `theme.ai: #7aa2f7` "
                 "in " + Config::default_path() + "\n  roles: " + theme_role_list() +
                 "  ·  colours: 0-255, #rrggbb, or a name (red, teal, amber, …)";
        return s;
    }
    if ( arg == "custom" && !has_custom )
        return "no custom palette configured — add `theme_base: <palette>` and one or more "
               "`theme.<role>: <colour>` lines to " + Config::default_path() +
               "\n  roles: " + theme_role_list() +
               "\n  colours: 0-255, #rrggbb, or a name (red, teal, amber, …)";
    if ( arg != "dark" && arg != "light" && arg != "warm" && arg != "cool" && arg != "rose" &&
         arg != "custom" )
        return "unknown theme: " + arg + "  (available: " + list + ")";
    _theme = build_theme(arg);
    _config.theme = _theme.name; // keep config in sync so the choice is persisted
    std::string s = "theme: " + _theme.name;
    if ( _theme.name == "custom" )
        s += "  (base " + _config.theme_base + " · " + theme_overrides_summary() + ")";
    return s;
}

std::string InlineRepl::theme_overrides_summary() const {
    // Roles in the canonical order, each shown IN its own colour so the summary
    // doubles as a preview of what was changed.
    std::string s;
    for ( const auto& role : theme_roles()) {
        auto it = _config.theme_colors.find(role);
        if ( it == _config.theme_colors.end())
            continue;
        Theme t = _theme;
        const std::string* slot = theme_role_slot(t, role);
        std::string color = slot ? *slot : "";
        s += ( s.empty() ? "" : " " ) + color + role + "=" + it->second + Theme::reset;
    }
    return s.empty() ? "(none)" : s;
}

InlineRepl::~InlineRepl() {
    if ( _worker.joinable())
        _worker.join();
    teardown();
}

void InlineRepl::setup() {
    if ( tcgetattr(STDIN_FILENO, &g_orig_termios) == 0 )
        g_termios_saved = true;

    struct termios raw = g_orig_termios;
    // Character-at-a-time input, no echo, no signal generation (we handle
    // Ctrl-C ourselves). Keep OPOST so '\n' still expands to CR-LF on output.
    raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | INLCR);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    wr("\033[?2004h"); // enable bracketed paste
    _raw_active = true;
}

void InlineRepl::teardown() {
    if ( !_raw_active )
        return;
    wr("\033[?2004l"); // disable bracketed paste
    wr("\033[?25h");   // ensure the cursor is visible
    wr("\033[0m");
    if ( g_termios_saved )
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    _raw_active = false;
}

void InlineRepl::emergency_teardown() {
    // Async-signal context: touch only the saved terminal state, no C++ objects.
    const char* reset = "\033[?2004l\033[?25h\033[0m";
    (void)::write(STDOUT_FILENO, reset, std::strlen(reset));
    if ( g_termios_saved )
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
}

int InlineRepl::term_cols() const {
    struct winsize ws;
    if ( ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 )
        return ws.ws_col;
    return 80;
}

int InlineRepl::term_rows() const {
    struct winsize ws;
    if ( ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 )
        return ws.ws_row;
    return 24;
}

// ── UTF-8 helpers ───────────────────────────────────────────────────────

size_t InlineRepl::prev_char(size_t pos) const {
    if ( pos == 0 )
        return 0;
    size_t i = pos - 1;
    while ( i > 0 && (static_cast<unsigned char>(_input[i]) & 0xC0) == 0x80 )
        --i;
    return i;
}

size_t InlineRepl::next_char(size_t pos) const {
    if ( pos >= _input.size())
        return _input.size();
    size_t i = pos + 1;
    while ( i < _input.size() && (static_cast<unsigned char>(_input[i]) & 0xC0) == 0x80 )
        ++i;
    return i;
}

int InlineRepl::display_width(const std::string& s) const {
    return static_cast<int>(split_cells(s).size());
}

// ── styling ─────────────────────────────────────────────────────────────

std::string InlineRepl::style_spans(const std::string& line, Language lang) const {
    auto spans = _highlighter.highlight(line, lang);
    if ( spans.empty())
        return line;

    std::string out;
    for ( const auto& sp : spans ) {
        std::string color;
        if ( sp.color_pair == _highlighter.color_for_keyword()) color = _theme.kw;
        else if ( sp.color_pair == _highlighter.color_for_string()) color = _theme.str;
        else if ( sp.color_pair == _highlighter.color_for_comment()) color = _theme.dim;
        else if ( sp.color_pair == _highlighter.color_for_number()) color = _theme.num;
        else if ( sp.color_pair == _highlighter.color_for_type()) color = _theme.type;
        else if ( sp.color_pair == _highlighter.color_for_fence()) color = _theme.dim;

        bool styled = !color.empty() || sp.bold || sp.underline;
        if ( styled ) {
            if ( sp.bold ) out += "\033[1m";
            if ( sp.underline ) out += "\033[4m";
            out += color;
        }
        out += sp.text;
        if ( styled )
            out += Theme::reset;
    }
    return out;
}

int InlineRepl::emit_styled_line(const std::string& line) {
    // Each reply line is left-padded 2 columns (matching the "> " on user
    // messages); combined with the wrap width below this leaves a 2-column right
    // margin. The very first line of a reply gets the AI marker instead of pad.
    auto next_prefix = [this]() -> std::string {
        if ( _reply_first_line ) {
            _reply_first_line = false;
            if ( _reply_dim )
                return thinking_style_for_theme(_theme) + "💭 " + Theme::reset;
            return _theme.ai + "● " + Theme::reset;
        }
        return "  ";
    };
    auto wrapped_rows = [this](const std::string& text, int prefix_cells = 2) -> int {
        int cols = term_cols();
        if ( cols < 1 )
            cols = 1;
        int cells = prefix_cells + static_cast<int>(split_cells(text).size());
        return std::max(1, ( cells + cols - 1 ) / cols);
    };

    // Thinking region: slightly brighter than generic dim/tool notices, no syntax
    // highlighting, word-wrapped like prose so it remains legible between tools.
    if ( _reply_dim ) {
        int width = term_cols() - 4;
        if ( width < 8 ) width = 8;
        std::vector<std::string> segs = word_wrap(line, width);
        int rows = 0;
        std::string think = thinking_style_for_theme(_theme);
        for ( size_t i = 0; i < segs.size(); ++i ) {
            wr(next_prefix() + think + segs[i] + Theme::reset);
            if ( i + 1 < segs.size())
                wr("\n");
            rows += wrapped_rows(segs[i]);
        }
        return rows;
    }

    std::string trimmed = common::trim_ws(line);
    if ( trimmed.rfind("```", 0) == 0 ) {
        // Fenced code block delimiter.
        if ( !_in_code ) {
            _in_code = true;
            _code_lang = _highlighter.detect(trimmed.substr(3));
        } else {
            _in_code = false;
            _code_lang = Language::none;
        }
        wr(next_prefix() + _theme.dim + line + "\033[0m");
        return wrapped_rows(line);
    }

    if ( _in_code ) {
        // Code is left unwrapped (breaking at spaces would be wrong); the
        // terminal soft-wraps it so a copy stays faithful.
        wr(next_prefix() + style_spans(line, _code_lang));
        return wrapped_rows(line);
    }

    // Prose: word-wrap so lines don't break mid-word, within the padded width.
    int width = term_cols() - 4;
    if ( width < 8 ) width = 8;
    std::vector<std::string> segs = word_wrap(line, width);
    int rows = 0;
    for ( size_t i = 0; i < segs.size(); ++i ) {
        wr(next_prefix() + style_spans(segs[i], Language::markdown));
        if ( i + 1 < segs.size())
            wr("\n");
        rows += wrapped_rows(segs[i]);
    }
    return rows;
}

// ── transcript output ───────────────────────────────────────────────────

// Drop blank lines from the start and end of a (possibly multi-line) message.
static std::string trim_blank_edges(const std::string& s) {
    std::vector<std::string> lines;
    std::string cur;
    for ( char c : s ) {
        if ( c == '\n' ) { lines.push_back(cur); cur.clear(); }
        else cur += c;
    }
    lines.push_back(cur);

    size_t b = 0, e = lines.size();
    while ( b < e && common::trim_ws(lines[b]).empty()) ++b;
    while ( e > b && common::trim_ws(lines[e - 1]).empty()) --e;

    std::string out;
    for ( size_t i = b; i < e; ++i ) {
        if ( i > b ) out += "\n";
        out += lines[i];
    }
    return out;
}

// Strip control characters (except tab) so pasted content can never corrupt the
// terminal when it is echoed into the transcript.
static std::string sanitize_display(const std::string& s) {
    std::string out;
    for ( unsigned char c : s ) {
        if ( c == '\t' || c >= 0x20 )
            out += static_cast<char>(c);
    }
    return out;
}

// Strip terminal control bytes (except tab) from CONTENT before it is written --
// most importantly BEL (0x07), which rings the terminal bell. A stray BEL in a
// tool's output or echoed back in a model reply would otherwise ring the bell
// outside the deliberate, level-gated bell policy (the source of "random" bells).
// Applied to raw content only, before any of our own ANSI colour codes are added;
// UTF-8 bytes (>= 0x80) pass through untouched, so multibyte text is preserved.
static std::string sanitize_control(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for ( unsigned char c : s )
        if ( c == '\t' || ( c >= 0x20 && c != 0x7f ))
            out += static_cast<char>(c);
    return out;
}

void InlineRepl::echo_user_multi(const std::vector<std::string>& parts) {
    erase_live();
    wr("\n"); // one blank line before the whole group, none between parts
    int width = term_cols() - 4;
    if ( width < 8 ) width = 8;
    for ( const auto& part : parts ) {
        std::string msg = trim_blank_edges(part);
        bool first = true;
        std::istringstream ls(msg);
        std::string logical;
        while ( std::getline(ls, logical)) {
            for ( const auto& seg : word_wrap(logical, width)) {
                if ( first ) {
                    wr(_theme.user + "› " + Theme::reset + seg + "\n");
                    first = false;
                } else {
                    wr("  " + seg + "\n");
                }
            }
        }
        if ( first ) // an empty part — still show a marker
            wr(_theme.user + "›" + Theme::reset + "\n");
    }
}

void InlineRepl::echo_user(const std::string& display) {
    erase_live();
    // One blank line before the message, edges trimmed — every message (user or
    // AI) is preceded by exactly one blank line so speakers are easy to tell apart.
    wr("\n");

    std::string msg = trim_blank_edges(display);
    int width = term_cols() - 4; // 2-col "> "/"  " prefix + 2-col right margin
    if ( width < 8 ) width = 8;

    bool first = true;
    auto emit = [&](const std::string& seg) {
        if ( first ) {
            wr(_theme.user + "› " + Theme::reset + seg + "\n");
            first = false;
        } else {
            wr("  " + seg + "\n"); // continuation / block lines align under the text
        }
    };

    auto emit_text = [&](const std::string& text) {
        std::istringstream ls(text);
        std::string logical;
        while ( std::getline(ls, logical))
            for ( const auto& seg : word_wrap(logical, width))
                emit(seg);
    };

    // Expand paste placeholders into framed blocks so the whole message (exactly
    // what the model received) is visible and copyable from the scrollback.
    size_t pos = 0;
    while ( pos <= msg.size()) {
        size_t best = std::string::npos;
        const PasteItem* which = nullptr;
        for ( const auto& p : _pastes ) {
            size_t f = msg.find(p.placeholder, pos);
            if ( f != std::string::npos && ( best == std::string::npos || f < best )) {
                best = f;
                which = &p;
            }
        }

        size_t text_end = ( best == std::string::npos ) ? msg.size() : best;
        if ( text_end > pos )
            emit_text(msg.substr(pos, text_end - pos));

        if ( which == nullptr )
            break;

        // Framed paste block.
        std::vector<std::string> plines;
        {
            std::istringstream cs(which->content);
            std::string cl;
            while ( std::getline(cs, cl))
                plines.push_back(cl);
        }
        std::string header = _theme.dim + "── pasted · " + std::to_string(plines.size()) + " lines ";
        int hw = static_cast<int>(split_cells("── pasted · " + std::to_string(plines.size()) + " lines ").size());
        for ( int i = hw; i < width; ++i ) header += "─";
        emit(header + "\033[0m");
        // Optionally preview only the first N lines, noting how many were hidden.
        // The full text is still what the model receives; this only trims the echo.
        size_t limit = _config.paste_preview;
        size_t shown = ( limit > 0 && plines.size() > limit ) ? limit : plines.size();
        for ( size_t i = 0; i < shown; ++i )
            for ( const auto& seg : word_wrap(sanitize_display(plines[i]), width))
                emit(_theme.dim + seg + "\033[0m");
        if ( shown < plines.size())
            emit(_theme.dim + "  … " + std::to_string(plines.size() - shown) + " more lines" + "\033[0m");
        std::string footer;
        for ( int i = 0; i < width; ++i ) footer += "─";
        emit(_theme.dim + footer + "\033[0m");

        pos = best + which->placeholder.size();
    }

    if ( first ) // empty message (shouldn't happen, but stay safe)
        wr(_theme.user + "›" + Theme::reset + "\n");
}

void InlineRepl::resume_last_exchange() {
    const auto& msgs = _conversation.messages();
    // The last assistant message that actually said something (skip tool-call-only
    // turns and empty content).
    int ai = -1;
    for ( int i = static_cast<int>(msgs.size()) - 1; i >= 0; --i )
        if ( msgs[i].role == Role::ASSISTANT && !common::trim_ws(msgs[i].content).empty()) { ai = i; break; }
    if ( ai < 0 )
        return; // nothing saved for this directory -- a fresh start

    // The user prompt that led to it, for context.
    int ui = -1;
    for ( int i = ai - 1; i >= 0; --i )
        if ( msgs[i].role == Role::USER ) { ui = i; break; }

    int width = term_cols() - 4;
    if ( width < 8 ) width = 8;

    wr("\n" + _theme.dim + "── resuming this project's last session · /clear to start fresh ──" + Theme::reset + "\n");

    // The prompt as a dim, single-line anchor (first line, clipped to width).
    // Sanitized: saved content can carry control bytes (e.g. a BEL echoed out of
    // tool output) that must never reach the terminal raw.
    if ( ui >= 0 ) {
        std::string p = common::trim_ws(msgs[ui].content);
        size_t nl = p.find('\n');
        if ( nl != std::string::npos ) p = p.substr(0, nl);
        p = sanitize_display(p); // after the line cut — this strips \n too
        auto cells = split_cells(p);
        if ( static_cast<int>(cells.size()) > width ) {
            std::string cut;
            for ( int i = 0; i < width - 1; ++i ) cut += cells[i];
            p = cut + "…";
        }
        wr("\n" + _theme.dim + "> " + p + Theme::reset + "\n");
    }

    // The reply, capped to the last N logical lines -- the tail (the conclusion /
    // the question it left you on) is what "where we left off" means -- rendered
    // through the normal reply path so wrapping / code fences / highlighting apply.
    std::vector<std::string> lines;
    {
        std::istringstream ls(msgs[ai].content);
        std::string ln;
        while ( std::getline(ls, ln)) lines.push_back(ln);
    }
    const size_t cap = 30;
    size_t start = ( lines.size() > cap ) ? lines.size() - cap : 0;
    if ( start > 0 )
        wr("\n" + _theme.dim + "  … earlier part omitted · /history for the full log" + Theme::reset + "\n");

    begin_reply();
    for ( size_t i = start; i < lines.size(); ++i )
        emit_reply_line(lines[i]);
    _in_reply = false;
    if ( _in_code ) { _in_code = false; _code_lang = Language::none; }
}

void InlineRepl::begin_reply() {
    _in_reply = true;
    _line_buf.clear();
    _in_code = false;
    _code_lang = Language::none;
    _pending_blanks = 0;
    _reply_has_content = false;
    _reply_first_line = true;
    _reply_dim = false;
    _notice_gap_done = false;
    _last_output_was_notice = false;
    _think_preview.clear();
    _stream_in_think = false;
}

void InlineRepl::emit_reply_line(const std::string& raw_line) {
    // Handle streamed thinking-region markers: \x01 opens (dim + 💭), \x02 closes
    // (back to the normal answer style + ● marker). Strip them from the text.
    std::string line = raw_line;
    size_t mp;
    while ( ( mp = line.find_first_of("\x01\x02")) != std::string::npos ) {
        _reply_dim = ( line[mp] == '\x01' );
        _reply_first_line = true; // the first line of each region gets its marker
        line.erase(mp, 1);
    }

    // Drop any stray terminal control byte (e.g. a BEL echoed from tool output)
    // so only the deliberate, level-gated bell can ever ring.
    line = sanitize_control(line);

    if ( common::trim_ws(line).empty()) {
        // Defer blank lines: interior ones are flushed once more content arrives;
        // leading ones are skipped; trailing ones are simply never flushed.
        if ( _reply_has_content )
            ++_pending_blanks;
        return;
    }

    if ( _last_output_was_notice )
        _pending_blanks = 0; // notice separators are handled explicitly; avoid stacked blank gaps

    int spacer_lines = 0;
    if ( !_reply_has_content ) {
        wr("\n");                 // the single blank line before the reply
        _reply_has_content = true;
        _last_output_was_notice = false;
        spacer_lines = 1;
    } else {
        if ( _last_output_was_notice ) {
            wr("\n");             // single separator between ⚙ group and next assistant text
            ++spacer_lines;
            _last_output_was_notice = false;
        }
        for ( int i = 0; i < _pending_blanks; ++i ) {
            wr("\n");
            ++spacer_lines;
        }
    }
    _pending_blanks = 0;

    int printed = emit_styled_line(line);
    wr("\n");

    if ( _turn_running )
        _live_cursor_up += printed + spacer_lines;
}

void InlineRepl::flush_lines() {
    // Emit every complete line from the buffer (the live block must already be
    // erased by the caller). A trailing partial line stays buffered until its
    // newline arrives, so highlighting always sees whole lines.
    size_t nl;
    while ( (nl = _line_buf.find('\n')) != std::string::npos ) {
        emit_reply_line(_line_buf.substr(0, nl));
        _line_buf.erase(0, nl + 1);
    }
}

void InlineRepl::route_stream_chunk(const std::string& chunk) {
    // Collapse mode: reasoning (between the \x01 and \x02 markers) is diverted to
    // the transient preview instead of _line_buf, so it is shown live but never
    // committed. \x02 (answer begins) drops the preview. Everything else is the
    // answer and flows into _line_buf as usual.
    std::string clean = sanitize_stream_text(chunk);
    for ( char ch : clean ) {
        if ( ch == '\x01' ) { _stream_in_think = true; continue; }
        if ( ch == '\x02' ) { _stream_in_think = false; _think_preview.clear(); continue; }
        if ( _stream_in_think )
            _think_preview += ch;
        else
            _line_buf += ch;
    }
}

std::vector<std::string> InlineRepl::think_preview_lines(int cols) const {
    std::vector<std::string> out;
    if ( _think_preview.empty())
        return out;

    int width = cols - 4;
    if ( width < 8 ) width = 8;

    // Word-wrap each logical line of the accumulated reasoning.
    std::vector<std::string> wrapped;
    size_t start = 0;
    while ( true ) {
        size_t nl = _think_preview.find('\n', start);
        std::string logical = ( nl == std::string::npos )
            ? _think_preview.substr(start)
            : _think_preview.substr(start, nl - start);
        for ( const auto& seg : word_wrap(logical, width))
            wrapped.push_back(seg);
        if ( nl == std::string::npos ) break;
        start = nl + 1;
    }
    if ( wrapped.empty())
        return out;

    // Bound the preview height (keep the most recent lines) so the live block
    // never grows without limit; the older reasoning is summarised in the header.
    int cap = 8;
    struct winsize ws;
    if ( ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 10 )
        cap = std::min(cap, static_cast<int>(ws.ws_row) - 8);
    if ( cap < 1 ) cap = 1;

    int total = static_cast<int>(wrapped.size());
    int first = ( total > cap ) ? ( total - cap ) : 0;

    std::string think = thinking_style_for_theme(_theme);
    std::string header = think + "💭 thinking";
    if ( first > 0 ) header += " (+" + std::to_string(first) + " earlier)";
    header += "…" + std::string(Theme::reset);
    out.push_back(header);
    for ( int i = first; i < total; ++i )
        out.push_back("  " + think + wrapped[i] + Theme::reset);
    return out;
}

// ── live block (input + status) ─────────────────────────────────────────

std::string InlineRepl::status_line() const {
    // While the AI is working, the status line becomes an activity indicator:
    // spinner + what it is doing + elapsed seconds (+ any queued messages).
    if ( _turn_running ) {
        static const char* frames[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
        const char* frame = frames[_spin % 10];

        std::string activity;
        bool streamed;
        size_t queued;
        size_t live_updates;
        std::string next_queued;
        {
            std::lock_guard<std::mutex> lk(_mx);
            activity = _activity;
            streamed = _turn_streamed;
            queued = _pending.size();
            live_updates = _live_updates.size();
            if ( queued > 0 )
                next_queued = _pending.front().text;
        }
        std::string what = !activity.empty() ? activity : ( streamed ? "responding" : "thinking");
        // The status MUST stay one line (erase_live assumes it). A multi-line
        // command (e.g. a heredoc) reaches here via "running: <command>", so flatten
        // any newline/tab/CR to a space before it is clipped — otherwise the extra
        // lines aren't erased and blank frames pile up between tool notices.
        for ( char& c : what )
            if ( c == '\n' || c == '\r' || c == '\t' ) c = ' ';

        auto secs = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - _turn_start).count();
        std::string secs_s = std::to_string(secs) + "s";

        // UTF-8-safe clip to N display columns, with an ellipsis when cut. Done on
        // the RAW text before any colour codes so no ANSI sequence is ever split.
        auto clip = [](const std::string& in, int limit) -> std::string {
            if ( limit < 1 ) return "";
            std::string out;
            int w = 0;
            size_t i = 0;
            while ( i < in.size()) {
                size_t j = i + 1;
                while ( j < in.size() && (static_cast<unsigned char>(in[j]) & 0xC0) == 0x80 ) ++j;
                if ( w + 1 > limit - 1 && j < in.size()) // room left for the ellipsis
                    return out + "…";
                out.append(in, i, j - i);
                ++w;
                i = j;
                if ( w >= limit )
                    return out;
            }
            return out;
        };

        // Queued messages: show the count and a peek at the next prompt so you
        // can see WHAT is waiting, not just how many.
        std::string qclip;
        if ( queued > 0 ) {
            PendingKind next_kind = PendingKind::Message;
            {
                std::lock_guard<std::mutex> lk(_mx);
                if ( !_pending.empty()) next_kind = _pending.front().kind;
            }
            std::string labelled = pending_kind_label(next_kind) + ": " + queue_preview(next_queued, 80);
            qclip = clip(labelled, 30);
        }
        std::string btw_suffix;
        if ( live_updates > 0 )
            btw_suffix = " · " + std::to_string(live_updates) + " btw";
        int tool_count = _turn_tool_count.load(std::memory_order_relaxed);
        std::string tool_suffix;
        if ( tool_count > 0 ) {
            tool_suffix = " · " + std::to_string(tool_count) + " tools";
            if ( _config.tool_call_limit > 0 )
                tool_suffix += "/" + std::to_string(_config.tool_call_limit);
        }

        // Everything must fit ONE terminal row: draw_live prints this verbatim and
        // erase_live assumes the status occupies exactly one line — an overflowing
        // status leaves stale frames behind on every spinner tick. When space is
        // tight, drop parts in priority order: the Ctrl-C hint first, then the
        // queued preview text, then the queued suffix entirely; `what` last.
        int cols = term_cols() - 1; // one column of margin: avoid the autowrap edge
        int fixed = 2 /* spinner+sp */ + 1 + static_cast<int>(secs_s.size());
        std::string hint = " (Ctrl-C to interrupt)";
        bool q_suffix = queued > 0, q_preview = queued > 0;

        auto qwidth = [&]() -> int {
            int w = 0;
            if ( !tool_suffix.empty())
                w += display_width(tool_suffix);
            if ( !btw_suffix.empty())
                w += display_width(btw_suffix);
            if ( !q_suffix ) return w;
            w += 3 + static_cast<int>(std::to_string(queued).size()) + 7; // " · N queued"
            if ( q_preview ) w += 3 + display_width(qclip) + 1;              // ": "...""
            return w;
        };
        auto room = [&]() { return cols - fixed - static_cast<int>(hint.size()) - qwidth(); };

        if ( room() < 12 ) hint.clear();
        if ( room() < 12 ) q_preview = false;
        if ( room() < 12 ) q_suffix = false;
        if ( room() < 12 ) tool_suffix.clear();
        if ( room() < 12 ) btw_suffix.clear();
        what = clip(what, std::max(1, room()));

        // Pre-styled: a bright spinner + label stands out against the dim idle
        // status line. (draw_live prints this verbatim while a turn is running.)
        std::string s = _theme.accent + std::string(frame) + Theme::reset + " "
                      + _theme.accent + what + " " + secs_s + Theme::reset;
        if ( !hint.empty())
            s += _theme.dim + hint + Theme::reset;
        if ( !btw_suffix.empty())
            s += _theme.dim + btw_suffix + Theme::reset;
        if ( !tool_suffix.empty())
            s += _theme.dim + tool_suffix + Theme::reset;
        if ( q_suffix ) {
            s += _theme.dim + " · " + Theme::reset + _theme.warn +
                 std::to_string(queued) + " queued" + Theme::reset;
            if ( q_preview )
                s += _theme.dim + ": “" + qclip + "”" + Theme::reset;
        }
        return s;
    }

    std::string cwd;
    try { cwd = std::filesystem::current_path().string(); } catch ( ... ) { cwd = "?"; }

    std::string tools = !_config.tools_enabled ? "tools off"
                        : _config.insecure ? "tools: insecure"
                        : (_config.confirm_tools ? "tools: confirm" : "tools: auto");
    if ( !_config.tool_profile.empty() && _config.tool_profile != "full" )
        tools += " [" + _config.tool_profile + "]";
    else if ( _config.plan_mode )
        tools += " · plan";

    std::string s = _config.provider + " · " + _config.model + " · " + cwd;
    // A named parallel session is shown so two windows on the same project are
    // never confused with each other; the default session adds nothing.
    if ( !_config.session_name.empty())
        s += " · [" + _config.session_name + "]";
    s += " · " + tools;

    // Token usage: current context size and cumulative session total.
    long ctx = _stats.context_tokens.load(std::memory_order_relaxed);
    long total = _stats.session_total();
    if ( ctx > 0 || total > 0 ) {
        auto fmt = [](long n) -> std::string {
            if ( n >= 1000 ) {
                long whole = n / 1000;
                long frac = (n % 1000) / 100;
                return std::to_string(whole) + "." + std::to_string(frac) + "k";
            }
            return std::to_string(n);
        };
        s += " · ctx " + fmt(ctx) + " · " + fmt(total) + " tok";
        double cost = _config.session_cost(_stats.session_input.load(std::memory_order_relaxed),
                                           _stats.session_output.load(std::memory_order_relaxed),
                                           _stats.session_cached.load(std::memory_order_relaxed),
                                           _stats.session_cache_creation.load(std::memory_order_relaxed));
        if ( cost >= 0 ) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), " · $%.4f", cost);
            s += buf;
        }
    }
    return s;
}

void InlineRepl::set_activity(const std::string& activity) {
    std::lock_guard<std::mutex> lk(_mx);
    _activity = activity;
}

void InlineRepl::notify(const std::string& line) {
    std::lock_guard<std::mutex> lk(_mx);
    _notices.push({ line, true });
}

void InlineRepl::notify_quiet(const std::string& line) {
    std::lock_guard<std::mutex> lk(_mx);
    _notices.push({ line, false });
}

void InlineRepl::notify_tool(const std::string& line) {
    _turn_tool_count.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(_mx);
    // The prefix identifies the operation and its target (for example
    // "read_file src/repl_inline.cpp"). Result sizes and timings are allowed to
    // differ without forcing a new visible row.
    size_t sep = line.find(" · ");
    std::string shown_key = sep == std::string::npos ? line : line.substr(0, sep);
    std::string key = shown_key;
    std::string range;
    size_t rb = shown_key.rfind(']');
    size_t lb = shown_key.rfind(" [");
    if ( lb != std::string::npos && rb == shown_key.size() - 1 ) {
        range = shown_key.substr(lb + 2, rb - lb - 2);
        key = shown_key.substr(0, lb);
    }
    if ( !_tool_rollup_key.empty() && key != _tool_rollup_key )
        flush_tool_rollup_locked();
    if ( _tool_rollup_key.empty()) {
        _tool_rollup_key = key;
        _tool_rollup_text = line;
        _tool_rollup_count = 1;
        if ( !range.empty()) _tool_rollup_ranges.push_back(range);
    } else {
        ++_tool_rollup_count;
        if ( !range.empty() && std::find(_tool_rollup_ranges.begin(), _tool_rollup_ranges.end(), range) == _tool_rollup_ranges.end())
            _tool_rollup_ranges.push_back(range);
    }
}

void InlineRepl::flush_tool_rollup_locked() {
    if ( _tool_rollup_count <= 0 ) return;
    if ( _tool_rollup_count == 1 )
        _notices.push({ _tool_rollup_text, false });
    else {
        std::string text = _tool_rollup_key + " · " + std::to_string(_tool_rollup_count) + " calls";
        if ( !_tool_rollup_ranges.empty()) {
            text += " · ranges ";
            for ( size_t i = 0; i < _tool_rollup_ranges.size() && i < 4; ++i )
                text += ( i ? ", " : "") + _tool_rollup_ranges[i];
            if ( _tool_rollup_ranges.size() > 4 ) text += ", …";
        }
        _notices.push({ text, false });
    }
    _tool_rollup_key.clear();
    _tool_rollup_text.clear();
    _tool_rollup_ranges.clear();
    _tool_rollup_count = 0;
}

std::vector<std::string> InlineRepl::take_live_updates() {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(_mx);
    while ( !_live_updates.empty()) {
        out.push_back(std::move(_live_updates.front()));
        _live_updates.pop_front();
    }
    return out;
}

bool InlineRepl::enqueue_prompt(const std::string& text) {
    std::lock_guard<std::mutex> lk(_mx);
    if ( _auto_since_user >= 2 )
        return false; // chain guard: results still fold in on the next user message
    ++_auto_since_user;
    _pending.push_back({ PendingKind::Message, text });
    return true;
}

void InlineRepl::enqueue_pending(std::string text, PendingKind kind) {
    std::lock_guard<std::mutex> lk(_mx);
    _pending.push_back({ kind, std::move(text) });
}

void InlineRepl::drain_notices() {
    std::vector<Notice> lines;
    {
        std::lock_guard<std::mutex> lk(_mx);
        flush_tool_rollup_locked();
        while ( !_notices.empty()) {
            lines.push_back(_notices.front());
            _notices.pop();
        }
    }
    if ( lines.empty())
        return;
    erase_live();
    // Every speaker block is preceded by exactly one blank line — including a
    // tool-notice (⚙) group that starts before any reply text has been printed.
    // If assistant text just printed, insert the same single separator before
    // the tool group; repeated notice batches stay visually grouped.
    if ( _turn_running && !_notice_gap_done ) {
        wr("\n");
        _notice_gap_done = true;
    } else if ( _turn_running && _reply_has_content && !_last_output_was_notice ) {
        wr("\n");
    }
    bool ring = false;
    for ( const auto& n : lines ) {
        std::string text = sanitize_control(n.text);
        if ( n.bell ) {
            wr(_theme.accent + "● " + text + Theme::reset + "\r\n");
            ring = true;
        } else {
            wr(_theme.dim + text + Theme::reset + "\r\n");
        }
    }
    _last_output_was_notice = true;
    if ( ring && bell_level(_config.bell) >= 3 ) // a notice (workflow done) is "attention"
        wr("\a"); // bell: the user may be looking elsewhere
    draw_live();
}

void InlineRepl::erase_live() {
    // Step up from the cursor to the block top (_live_cursor_up lines) before
    // clearing, so the whole block is removed regardless of its height.
    if ( _live_lines > 0 )
        wr("\r\033[" + std::to_string(_live_cursor_up) + "A\033[J");
    else
        wr("\r\033[J");
    _live_lines = 0;
}

// Wrap any whole-word "ultracode"/"ultrathink" occurrence in a bold magenta so the
// user notices the effort/budget-raising marker. Operates on already-rendered line
// text; the colour codes carry zero display width, so cursor maths are unaffected.
std::string InlineRepl::highlight_keywords(const std::string& body) const {
    static const std::vector<std::string> kws = { "ultracode", "ultrathink" };
    std::string lo;
    lo.reserve(body.size());
    for ( unsigned char c : body ) lo += static_cast<char>(std::tolower(c));

    const std::string colour = "\033[1;35m"; // bold magenta — a deliberate budget cue
    std::string out;
    size_t i = 0;
    while ( i < body.size()) {
        bool hit = false;
        for ( const auto& kw : kws ) {
            if ( i + kw.size() <= body.size() && lo.compare(i, kw.size(), kw) == 0 ) {
                bool lb = ( i == 0 ) || !std::isalnum(static_cast<unsigned char>(body[i - 1]));
                size_t end = i + kw.size();
                bool rb = ( end >= body.size()) || !std::isalnum(static_cast<unsigned char>(body[end]));
                if ( lb && rb ) {
                    out += colour + body.substr(i, kw.size()) + "\033[0m";
                    i = end;
                    hit = true;
                    break;
                }
            }
        }
        if ( !hit ) { out += body[i]; ++i; }
    }
    return out;
}

std::vector<std::pair<size_t, size_t>> InlineRepl::wrap_input(int width) const {
    if ( width < 1 ) width = 1;
    std::vector<std::pair<size_t, size_t>> lines;
    size_t line_start = 0;
    int col = 0;
    size_t last_space = std::string::npos; // byte pos just after the last space on this visual line
    size_t i = 0;

    auto codepoints_between = [this](size_t a, size_t b) {
        int n = 0;
        for ( size_t k = a; k < b; ++k )
            if ( (static_cast<unsigned char>(_input[k]) & 0xC0) != 0x80 ) ++n;
        return n;
    };

    while ( i < _input.size()) {
        if ( _input[i] == '\n' ) {
            lines.push_back({ line_start, i });
            ++i;
            line_start = i;
            col = 0;
            last_space = std::string::npos;
            continue;
        }
        size_t j = i + 1;
        while ( j < _input.size() && (static_cast<unsigned char>(_input[j]) & 0xC0) == 0x80 )
            ++j;
        if ( col == width ) {                 // must wrap before this codepoint
            if ( last_space != std::string::npos && last_space > line_start && last_space <= i ) {
                // Break after the last space so words stay whole; carry the rest
                // of the current word to the next visual line.
                lines.push_back({ line_start, last_space });
                col = codepoints_between(last_space, i);
                line_start = last_space;
            } else {
                // A single over-long word (or no space yet): hard-break here.
                lines.push_back({ line_start, i });
                col = 0;
                line_start = i;
            }
            last_space = std::string::npos;
        }
        ++col;
        if ( _input[i] == ' ' )
            last_space = j;   // a break may start just after this space
        i = j;
    }
    lines.push_back({ line_start, _input.size() });
    return lines;
}

// Locate _cursor within a wrapped layout: row index and display column.
static void locate_cursor(const std::vector<std::pair<size_t, size_t>>& lines,
                          const std::string& input, size_t cursor, int& row, int& col) {
    auto width = [](const std::string& s) {
        int w = 0;
        for ( unsigned char ch : s )
            if ( (ch & 0xC0) != 0x80 ) ++w;
        return w;
    };
    for ( size_t r = 0; r < lines.size(); ++r ) {
        bool last = ( r + 1 == lines.size());
        bool wrap_next = !last && lines[r + 1].first == lines[r].second;
        if ( cursor < lines[r].second || ( cursor == lines[r].second && !wrap_next )) {
            row = static_cast<int>(r);
            col = width(input.substr(lines[r].first, cursor - lines[r].first));
            return;
        }
        if ( last ) {
            row = static_cast<int>(r);
            col = width(input.substr(lines[r].first, cursor - lines[r].first));
        }
    }
}

bool InlineRepl::multiline_vertical(int dir) {
    int width = term_cols() - 2 - 1;
    if ( width < 1 ) width = 1;
    auto lines = wrap_input(width);
    if ( lines.size() <= 1 )
        return false; // single visual line — let history handle Up/Down

    int row = 0, col = 0;
    locate_cursor(lines, _input, _cursor, row, col);

    int target = row + dir;
    if ( target < 0 || target >= static_cast<int>(lines.size()))
        return false; // at the top / bottom edge

    // Walk `col` display columns into the target line.
    size_t off = lines[target].first;
    int c = 0;
    while ( off < lines[target].second && c < col ) {
        size_t j = off + 1;
        while ( j < _input.size() && (static_cast<unsigned char>(_input[j]) & 0xC0) == 0x80 )
            ++j;
        off = j;
        ++c;
    }
    _cursor = off;
    return true;
}

void InlineRepl::draw_live() {
    if ( _defer_draw )
        return; // a burst is being fed; the main loop draws once at the end
    int cols = term_cols();
    const int prefix_w = 2;

    // Build the input as one or more visual lines, each (prefix, body), plus the
    // cursor's row within them and display column (excluding the prefix).
    std::vector<std::pair<std::string, std::string>> vlines;
    int cur_row = 0, cur_col = 0;

    if ( !_config.multiline ) {
        // Single line with a horizontal window; "…" marks clipped ends and one
        // right-hand column stays blank to avoid the terminal's auto-wrap.
        std::vector<std::string> in_cells = split_cells(_input);
        int total = static_cast<int>(in_cells.size());
        int cursor_pos = display_width(_input.substr(0, _cursor));
        int win = cols - prefix_w - 1;
        if ( win < 1 ) win = 1;
        int start = static_cast<int>(_input_window_start);
        if ( total <= win ) start = 0;
        else {
            if ( cursor_pos < start ) start = cursor_pos;
            else if ( cursor_pos > start + win - 1 ) start = cursor_pos - (win - 1);
            if ( start > total - win ) start = total - win;
            if ( start < 0 ) start = 0;
        }
        _input_window_start = static_cast<size_t>(start);
        bool clip_left = start > 0;
        bool clip_right = start + win < total;
        std::vector<std::string> vis;
        for ( int i = start; i < start + win && i < total; ++i )
            vis.push_back(in_cells[i]);
        if ( clip_left && !vis.empty()) vis.front() = "…";
        if ( clip_right && !vis.empty()) vis.back() = "…";
        std::string body;
        for ( const auto& ch : vis ) {
            if ( ch == "\n" ) body += _theme.dim + "↵" + Theme::reset; // newline glyph
            else body += ch;
        }
        vlines.push_back({ "> ", body });
        cur_row = 0;
        cur_col = cursor_pos - start;
    } else {
        // Multi-line: wrap the whole input; window vertically around the cursor.
        int width = cols - prefix_w - 1;
        if ( width < 1 ) width = 1;
        auto ranges = wrap_input(width);
        int full_row = 0;
        locate_cursor(ranges, _input, _cursor, full_row, cur_col);

        int nrows = static_cast<int>(ranges.size());
        int maxrows = 12;
        struct winsize ws;
        if ( ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 8 )
            maxrows = std::min(maxrows, static_cast<int>(ws.ws_row) - 6);
        if ( maxrows < 3 ) maxrows = 3;

        int first = 0, count = nrows;
        if ( nrows > maxrows ) {
            first = full_row - maxrows / 2;
            if ( first < 0 ) first = 0;
            if ( first > nrows - maxrows ) first = nrows - maxrows;
            count = maxrows;
        }
        cur_row = full_row - first;
        for ( int r = first; r < first + count; ++r ) {
            std::string body = _input.substr(ranges[r].first, ranges[r].second - ranges[r].first);
            std::string pfx;
            if ( r == 0 ) pfx = "> ";
            else if ( r == first && first > 0 ) pfx = _theme.dim + "⋮ " + Theme::reset; // more above
            else pfx = "  ";
            vlines.push_back({ pfx, body });
        }
    }

    // Status line. While a turn runs it is pre-styled (bright spinner) and short,
    // so it is printed verbatim; the idle status is plain text, clipped to leave
    // the last column blank and dimmed below.
    std::string status = status_line();
    bool status_prestyled = _turn_running;
    if ( !status_prestyled ) {
        std::vector<std::string> scells = split_cells(status);
        int limit = cols - 1;
        if ( limit > 0 && static_cast<int>(scells.size()) > limit ) {
            std::string clipped;
            for ( int i = 0; i < limit; ++i )
                clipped += scells[i];
            status = clipped;
        }
    }

    std::string sep;
    for ( int i = 0; i < cols; ++i )
        sep += "─";

    // Block layout: blank spacer, separator, the K input lines, separator, status.
    // Collapse mode: a transient reasoning preview sits at the very top of the
    // live block. It is redrawn as reasoning streams and erased with the block
    // when the answer arrives or the turn ends — never entering the transcript.
    std::vector<std::string> preview;
    if ( _turn_running && _config.thinking_collapse && _config.thinking_stream )
        preview = think_preview_lines(cols);
    int P = static_cast<int>(preview.size());
    // A blank line separates the user's prompt above from the reasoning preview.
    int preview_rows = ( P > 0 ) ? ( P + 1 ) : 0;

    int K = static_cast<int>(vlines.size());
    std::string out = ( _live_lines > 0 ) ? ("\r\033[" + std::to_string(_live_cursor_up) + "A\033[J")
                                          : "\r\033[J";
    if ( P > 0 )
        out += "\r\n";                        // blank line above the reasoning preview
    for ( const auto& pl : preview )
        out += pl + "\r\n";                   // transient reasoning preview
    out += "\r\n";                            // blank spacer above the separator
    out += _theme.dim + sep + "\033[0m\r\n";  // separator: transcript | input
    // The ultracode/ultrathink markers raise Anthropic's effort to max for a turn
    // (and cost budget), so flag them in a distinct colour where they take effect.
    bool mark_ultra = ( _config.provider == "claude" || _config.provider == "anthropic" );
    for ( const auto& vl : vlines )
        out += vl.first + ( mark_ultra ? highlight_keywords(vl.second) : vl.second ) + "\r\n";
    out += _theme.dim + sep + "\033[0m\r\n";  // separator: input | status
    out += status_prestyled ? status : (_theme.dim + status + "\033[0m"); // status

    out += "\033[" + std::to_string(1 + K - cur_row) + "A\r"; // status up to the cursor's input line
    int screen_col = prefix_w + cur_col;
    if ( screen_col > 0 )
        out += "\033[" + std::to_string(screen_col) + "C";
    wr(out);
    _live_lines = 4 + K + preview_rows;
    _live_cursor_up = 2 + cur_row + preview_rows;
}

// ── input editing ───────────────────────────────────────────────────────

void InlineRepl::insert_text(const std::string& text) {
    _input.insert(_cursor, text);
    _cursor += text.size();
}

std::pair<size_t, size_t> InlineRepl::placeholder_ending_at(size_t pos) const {
    for ( const auto& p : _pastes ) {
        size_t n = p.placeholder.size();
        if ( n <= pos && _input.compare(pos - n, n, p.placeholder) == 0 )
            return { pos - n, pos };
    }
    return { std::string::npos, std::string::npos };
}

std::pair<size_t, size_t> InlineRepl::placeholder_starting_at(size_t pos) const {
    for ( const auto& p : _pastes ) {
        size_t n = p.placeholder.size();
        if ( pos + n <= _input.size() && _input.compare(pos, n, p.placeholder) == 0 )
            return { pos, pos + n };
    }
    return { std::string::npos, std::string::npos };
}

void InlineRepl::drop_paste(const std::string& placeholder) {
    _pastes.erase(std::remove_if(_pastes.begin(), _pastes.end(),
                                 [&](const PasteItem& p) { return p.placeholder == placeholder; }),
                  _pastes.end());
}

void InlineRepl::backspace() {
    if ( _cursor == 0 )
        return;
    // A paste box is atomic: deleting from just after it removes the whole box.
    auto box = placeholder_ending_at(_cursor);
    if ( box.first != std::string::npos ) {
        std::string token = _input.substr(box.first, box.second - box.first);
        _input.erase(box.first, box.second - box.first);
        _cursor = box.first;
        drop_paste(token);
        return;
    }
    size_t p = prev_char(_cursor);
    _input.erase(p, _cursor - p);
    _cursor = p;
}

namespace {

const std::vector<std::string>& slash_commands() {
    static const std::vector<std::string> cmds = {
        "/help", "/about", "/info", "/settings", "/provider", "/model", "/profile", "/btw", "/note", "/steer",
        "/tools", "/strict", "/thinking", "/effort", "/theme", "/stream", "/bell",
        "/memories", "/roadmap", "/context", "/cost", "/history", "/retry", "/undo", "/tasks",
        "/pin", "/pins", "/unpin", "/queue", "/trust", "/skills", "/skill", "/plan",
        "/changes", "/export", "/compact", "/clear", "/reset", "/mcp", "/advisor",
        "/autoresume", "/paste", "/raw", "/limits", "/jobs", "/workflows",
        "/sessions", "/session", "/shell", "/exit", "/quit"
    };
    return cmds;
}

std::string common_prefix(const std::vector<std::string>& v) {
    if ( v.empty()) return "";
    std::string p = v[0];
    for ( size_t i = 1; i < v.size(); ++i ) {
        size_t k = 0;
        while ( k < p.size() && k < v[i].size() && p[k] == v[i][k] ) ++k;
        p.resize(k);
    }
    return p;
}

} // namespace

// Tab completion: complete a leading slash command, or a file-path token.
void InlineRepl::handle_tab() {
    // The token being completed runs from the last whitespace before the cursor.
    size_t start = _cursor;
    while ( start > 0 && _input[start - 1] != ' ' && _input[start - 1] != '\n' )
        --start;
    std::string token = _input.substr(start, _cursor - start);

    std::vector<std::string> matches;   // full replacement text for the token
    std::vector<std::string> display;   // what to show in an ambiguity list

    bool is_command = ( start == 0 && !token.empty() && token[0] == '/' );
    // "@path" file mentions: complete the path part, keep the '@' prefix.
    std::string at_prefix;
    if ( !token.empty() && token[0] == '@' ) {
        at_prefix = "@";
        token.erase(0, 1);
    }
    if ( is_command ) {
        for ( const auto& c : slash_commands())
            if ( c.rfind(token, 0) == 0 ) { matches.push_back(c); display.push_back(c); }
    } else {
        std::string prefix_before = common::trim_ws(_input.substr(0, start));
        std::vector<std::string> sub_candidates;
        if ( prefix_before == "/profile" || prefix_before == "/tools profile" ) {
            sub_candidates = { "full", "code", "research", "review", "minimal" };
        } else if ( prefix_before == "/tools" ) {
            sub_candidates = { "confirm", "auto", "insecure", "list", "group", "profile" };
        } else if ( prefix_before == "/tools group" ) {
            sub_candidates = { "core", "web", "workflow", "skills", "mcp" };
        } else if ( prefix_before.rfind("/tools group ", 0) == 0 ) {
            sub_candidates = { "on", "off" };
        } else if ( prefix_before == "/mcp" ) {
            sub_candidates = { "refresh", "prompt", "enable", "disable" };
        } else if ( prefix_before == "/mcp enable" || prefix_before == "/mcp disable" ) {
            if ( _mcp_provider ) {
                for ( const auto& s : _mcp_provider())
                    sub_candidates.push_back(s.name);
            }
        }

        if ( !sub_candidates.empty()) {
            for ( const auto& c : sub_candidates ) {
                if ( token.empty() || c.rfind(token, 0) == 0 ) {
                    matches.push_back(c);
                    display.push_back(c);
                }
            }
        }
    }

    if ( matches.empty() && ( !token.empty() || !at_prefix.empty())) {
        // Path completion: split into a directory part and a name prefix.
        std::string dir, prefix;
        size_t slash = token.rfind('/');
        if ( slash == std::string::npos ) { dir = ""; prefix = token; }
        else { dir = token.substr(0, slash + 1); prefix = token.substr(slash + 1); }
        std::string fsdir = dir.empty() ? "." : dir;
        std::error_code ec;
        if ( std::filesystem::is_directory(fsdir, ec)) {
            for ( const auto& e : std::filesystem::directory_iterator(
                      fsdir, std::filesystem::directory_options::skip_permission_denied, ec)) {
                std::string name = e.path().filename().string();
                if ( name.empty()) continue;
                // Hidden files only when the prefix explicitly starts with '.'.
                if ( name[0] == '.' && ( prefix.empty() || prefix[0] != '.' )) continue;
                if ( name.rfind(prefix, 0) != 0 ) continue;
                std::error_code dec;
                bool is_dir = e.is_directory(dec);
                matches.push_back(dir + name + ( is_dir ? "/" : "" ));
                display.push_back(name + ( is_dir ? "/" : "" ));
            }
            std::sort(matches.begin(), matches.end());
            std::sort(display.begin(), display.end());
        }
    }

    if ( matches.empty())
        return; // nothing to complete

    auto replace_token = [&](const std::string& text, bool add_space) {
        _input.erase(start, _cursor - start);
        std::string ins = at_prefix + text + ( add_space ? " " : "" );
        _input.insert(start, ins);
        _cursor = start + ins.size();
        _input_window_start = 0;
    };

    if ( matches.size() == 1 ) {
        // A directory completion keeps going (no trailing space); commands and
        // files get a space so the next token can be typed.
        bool is_dir = !matches[0].empty() && matches[0].back() == '/';
        replace_token(matches[0], !is_dir);
        draw_live();
        return;
    }

    // Multiple: extend to the longest common prefix if that adds anything…
    std::string cp = common_prefix(matches);
    if ( cp.size() > token.size()) {
        replace_token(cp, false);
        draw_live();
        return;
    }

    // …otherwise show the candidates above the input.
    erase_live();
    int cols = term_cols();
    std::string line;
    for ( const auto& d : display ) {
        if ( !line.empty() && static_cast<int>(line.size() + d.size() + 2) > cols ) {
            wr(_theme.dim + line + Theme::reset + "\n");
            line.clear();
        }
        line += ( line.empty() ? "" : "  " ) + d;
    }
    if ( !line.empty())
        wr(_theme.dim + line + Theme::reset + "\n");
    draw_live();
}

// After deleting a span that may have contained paste placeholders, forget any
// whose token no longer appears in the input (so their content isn't re-sent).
void InlineRepl::prune_pastes() {
    for ( auto it = _pastes.begin(); it != _pastes.end(); ) {
        if ( _input.find(it->placeholder) == std::string::npos )
            it = _pastes.erase(it);
        else
            ++it;
    }
}

// Ctrl-W: delete the whitespace-delimited word before the cursor (skip trailing
// spaces, then remove back to the previous whitespace boundary).
void InlineRepl::delete_word_before() {
    if ( _cursor == 0 )
        return;
    // A paste box just before the cursor is atomic — remove the whole box, like
    // backspace, rather than word-deleting into "[paste #1: 5 " and corrupting it.
    auto box = placeholder_ending_at(_cursor);
    if ( box.first != std::string::npos ) {
        std::string token = _input.substr(box.first, box.second - box.first);
        _input.erase(box.first, box.second - box.first);
        _cursor = box.first;
        drop_paste(token);
        return;
    }
    auto is_space = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\n'; };
    size_t p = _cursor;
    while ( p > 0 && is_space(_input[p - 1]) ) --p;
    while ( p > 0 && !is_space(_input[p - 1]) ) --p;
    // If the word-delete span would cut INTO a paste box (whose text contains
    // spaces), extend the start to the box start so the box is removed whole.
    for ( const auto& pi : _pastes ) {
        size_t ps = _input.find(pi.placeholder);
        if ( ps != std::string::npos && ps < p && ps + pi.placeholder.size() > p )
            p = ps;
    }
    _input.erase(p, _cursor - p);
    _cursor = p;
    prune_pastes();
}

// Ctrl-U: delete from the start of the current line (after the previous newline)
// up to the cursor.
void InlineRepl::kill_to_line_start() {
    if ( _cursor == 0 )
        return;
    size_t nl = _input.rfind('\n', _cursor - 1);
    size_t start = ( nl == std::string::npos ) ? 0 : nl + 1;
    _input.erase(start, _cursor - start);
    _cursor = start;
    prune_pastes();
}

// Ctrl-K: delete from the cursor to the end of the current line (next newline).
void InlineRepl::kill_to_line_end() {
    if ( _cursor >= _input.size())
        return;
    size_t nl = _input.find('\n', _cursor);
    size_t end = ( nl == std::string::npos ) ? _input.size() : nl;
    _input.erase(_cursor, end - _cursor);
    prune_pastes();
}

void InlineRepl::move_left() {
    if ( _cursor == 0 )
        return;
    auto box = placeholder_ending_at(_cursor);
    _cursor = ( box.first != std::string::npos ) ? box.first : prev_char(_cursor);
}

void InlineRepl::move_right() {
    if ( _cursor >= _input.size())
        return;
    auto box = placeholder_starting_at(_cursor);
    _cursor = ( box.second != std::string::npos ) ? box.second : next_char(_cursor);
}

void InlineRepl::history_prev() {
    if ( _prompt_history.empty() || _history_index == 0 )
        return;
    if ( _history_index == _prompt_history.size())
        _stashed_input = _input; // stash the line being edited
    --_history_index;
    _input = _prompt_history[_history_index];
    _cursor = _input.size();
}

void InlineRepl::history_next() {
    if ( _history_index >= _prompt_history.size())
        return;
    ++_history_index;
    if ( _history_index == _prompt_history.size())
        _input = _stashed_input;
    else
        _input = _prompt_history[_history_index];
    _cursor = _input.size();
}

std::string InlineRepl::substitute_pastes(std::string out) const {
    for ( const auto& p : _pastes ) {
        size_t pos;
        while ( (pos = out.find(p.placeholder)) != std::string::npos )
            out.replace(pos, p.placeholder.size(), p.content);
    }
    return out;
}

std::string InlineRepl::expand_input() const {
    return substitute_pastes(_input);
}

void InlineRepl::read_bracketed_paste() {
    // Collect bytes until the closing "\033[201~".
    std::string content;
    const std::string terminator = "\033[201~";
    while ( true ) {
        int c = read_byte();
        if ( c < 0 )
            break;
        content += static_cast<char>(c);
        if ( content.size() >= terminator.size() &&
             content.compare(content.size() - terminator.size(), terminator.size(), terminator) == 0 ) {
            content.erase(content.size() - terminator.size());
            break;
        }
    }

    // Normalise CR / CRLF to LF.
    std::string norm;
    for ( size_t i = 0; i < content.size(); ++i ) {
        if ( content[i] == '\r' ) {
            norm += '\n';
            if ( i + 1 < content.size() && content[i + 1] == '\n' )
                ++i;
        } else {
            norm += content[i];
        }
    }

    size_t lines = static_cast<size_t>(std::count(norm.begin(), norm.end(), '\n'));
    if ( norm.empty() || norm.back() != '\n' )
        ++lines; // count the final line only when there is no trailing newline
    // Only large pastes collapse into a box. A short multi-line paste is inserted
    // inline; its newlines survive as atomic "↵" glyphs in the prompt.
    bool large = norm.size() > _config.paste_threshold_chars ||
                 lines > _config.paste_threshold_lines;

    if ( large ) {
        ++_paste_counter;
        PasteItem item;
        item.placeholder = "[paste #" + std::to_string(_paste_counter) + ": " +
                           std::to_string(lines) + " lines]";
        item.content = norm;
        _pastes.push_back(item);
        insert_text(item.placeholder);
    } else {
        insert_text(norm);
    }
}

// ── run loop ────────────────────────────────────────────────────────────

void InlineRepl::run() {
    setup();

    // Start on a clean screen so pre-launch shell output doesn't clutter the
    // transcript (also clears scrollback so scrolling up shows only this session).
    wr("\033[H\033[2J\033[3J");

    wr("\033[1magent\033[0m — " + _config.provider + " · " + _config.model + "\n");
    wr(_theme.dim + "Type your message. /exit or /quit to leave, Ctrl-C to interrupt." + Theme::reset + "\n");

    // Show the previous session's last exchange (if this directory has one) so a
    // resumed conversation opens where it left off instead of on a blank screen.
    resume_last_exchange();

    // A nearly-full disk endangers the session file itself — surface it before
    // the first save, not after it already failed.
    {
        std::string dwarn = disk_space_warning();
        if ( !dwarn.empty())
            wr("\n" + _theme.warn + "⚠ " + dwarn + Theme::reset + "\n");
    }

    _history_index = _prompt_history.size();
    draw_live();

    // Terminal is now in raw mode: safe to run any startup confirm (e.g. approving
    // a project-local MCP server) before entering the event loop.
    if ( _on_ready ) {
        _on_ready();
        _on_ready = nullptr;
        draw_live();
    }

    // Event loop: the LLM turn runs on a worker thread, so here we just service
    // the keyboard and the worker's output/confirm queues. select() gives a
    // short timeout that both keeps the spinner animating and lets streamed
    // output appear promptly.
    while ( agent::running.load(std::memory_order_relaxed)) {
        poll_worker();

        // Async notices (workflow completions etc.) print above the live block;
        // held back while a dialog owns the screen.
        if ( !_confirming && !_asking && !_in_settings && !_in_list )
            drain_notices();

        // Idle + something queued (e.g. a workflow auto-resume prompt enqueued
        // from a background thread): run it through the normal turn machinery.
        if ( !_turn_running && !_confirming && !_asking && !_in_settings && !_in_list ) {
            bool has;
            {
                std::lock_guard<std::mutex> lk(_mx);
                has = !_pending.empty();
            }
            if ( has )
                drain_pending();
        }

        // Terminal was resized: redraw the active view at the new width.
        if ( agent::winch_pending.exchange(false, std::memory_order_relaxed)) {
            if ( _in_list )
                draw_list_menu(true); // back up over the old render, don't stack it
            else if ( _in_settings )
                draw_settings_menu(true); // redraw=true so it doesn't stack a copy
            else if ( _asking )
                draw_ask_menu(true);      // reflow the ask dialog, don't draw over it
            else if ( !_confirming )
                draw_live();
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv { 0, 80 * 1000 }; // 80 ms
        int r = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);

        if ( r > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
            int c = read_byte();
            if ( c < 0 ) {
                if ( errno == EINTR )
                    continue;
                break; // EOF
            }
            if ( _confirming )
                handle_confirm_key(c);
            else if ( _asking )
                handle_ask_key(c);
            else if ( _in_list )
                handle_list_key(c);
            else if ( _in_settings )
                handle_settings_key(c);
            else {
                // If a lot of input is already buffered (an unbracketed paste),
                // feed it all with redraws deferred and draw once at the end —
                // per-byte redraws make a large paste quadratically slow.
                int avail = 0;
                if ( ioctl(STDIN_FILENO, FIONREAD, &avail) != 0 )
                    avail = 0;
                if ( avail > 64 ) {
                    _defer_draw = true;
                    handle_byte(c);
                    long budget = 1 << 20; // hard cap per loop iteration
                    while ( budget-- > 0 && !_confirming && !_asking && !_in_settings && !_in_list ) {
                        int rem = 0;
                        if ( ioctl(STDIN_FILENO, FIONREAD, &rem) != 0 || rem <= 0 )
                            break;
                        int b = read_byte();
                        if ( b < 0 )
                            break;
                        handle_byte(b);
                    }
                    _defer_draw = false;
                    // A byte-fed command may have opened a modal mid-burst (the loop
                    // stops for that) — don't draw the live block over it.
                    if ( !_confirming && !_asking && !_in_settings && !_in_list )
                        draw_live();
                } else {
                    handle_byte(c);
                }
            }
        } else if ( r < 0 && errno != EINTR ) {
            break;
        } else if ( _turn_running && !_confirming && !_asking && !_in_settings && !_in_list ) {
            // Idle tick while working: advance the spinner.
            ++_spin;
            draw_live();
        }
    }

    // Tear down any in-flight turn cleanly.
    agent::turn_abort.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(_mx);
        _confirm_decision = tools::Decision::deny;
        _confirm_answered = true;
        _ask_answered = true; // release a worker blocked in ask_user() too, else join() hangs
    }
    _cv.notify_all();
    if ( _worker.joinable())
        _worker.join();

    teardown();
    wr("\n");
}

void InlineRepl::on_enter() {
    std::string display = _input;

    // @path file mentions expand for MESSAGES only (never inside slash commands),
    // and before paste substitution so @tokens inside pasted content stay text.
    bool is_cmd = !common::trim_ws(_input).empty() && common::trim_ws(_input)[0] == '/';
    std::vector<agent::FileMention> fmentions;
    std::string pre = is_cmd ? _input : agent::expand_file_mentions(_input, &fmentions);
    std::string line = substitute_pastes(pre);
    std::string trimmed = common::trim_ws(line);
    for ( const auto& m : fmentions )
        notify_quiet("@" + m.path + " attached (" + std::to_string(m.lines) + " lines" +
                     ( m.truncated ? ", truncated" : "" ) + ")");

    if ( trimmed.empty()) {
        _input.clear();
        _cursor = 0;
        _input_window_start = 0;
        draw_live();
        return;
    }
    if ( trimmed == "/exit" || trimmed == "/quit" ) {
        agent::running.store(false, std::memory_order_relaxed);
        return;
    }

    // Slash commands and !shell passthroughs run locally (never sent to the
    // model as prompts), even mid-turn.
    if ( trimmed[0] == '/' || trimmed[0] == '!' ) {
        _prompt_history.push_back(trimmed);
        _history_index = _prompt_history.size();
        _stashed_input.clear();
        _input.clear();
        _cursor = 0;
        _input_window_start = 0;
        _pastes.clear();

        // /queue inspects or edits the pending queue itself, so it must run NOW,
        // not be appended to the very queue it manages.
        if ( trimmed == "/queue" || trimmed.rfind("/queue ", 0) == 0 ) {
            queue_command(trimmed);
            return;
        }

        // Local/display-only commands (menus, readers, UI settings) don't touch
        // the running conversation, model or provider, so they run NOW even
        // mid-turn — like /queue. Everything that mutates the conversation or
        // starts work (clear, undo, compact, model/provider switch, …) queues so
        // it runs when the turn finishes, instead of interleaving with the output.
        if ( _turn_running && !command_runs_immediately(trimmed)) {
            bool is_steer = trimmed.rfind("/steer", 0) == 0;
            PendingKind kind = ( is_steer || trimmed.rfind("/btw", 0) == 0 || trimmed.rfind("/note", 0) == 0 )
                ? PendingKind::LiveNote : ( trimmed[0] == '!' ? PendingKind::Shell : PendingKind::Command );
            if ( kind == PendingKind::LiveNote ) {
                std::lock_guard<std::mutex> lk(_mx);
                _live_updates.push_back(trimmed);
                _notices.push({ is_steer ? "steering update queued for the next checkpoint"
                                         : "btw update queued for the next checkpoint", false });
            } else {
                enqueue_pending(trimmed, kind);
            }
            tcflush(STDIN_FILENO, TCIFLUSH);
            draw_live();
            return;
        }
        if ( trimmed.rfind("/steer", 0) == 0 ) {
            std::string prompt = common::trim_ws(trimmed.substr(6));
            if ( prompt.empty()) {
                echo_user(trimmed);
                wr(_theme.warn + "usage: /steer <prompt>  — steer active work at the next checkpoint, or send as a prompt\n" + Theme::reset);
                _input.clear(); _cursor = 0; _input_window_start = 0;
                draw_live();
                return;
            }
            _prompt_history.push_back(line);
            _history_index = _prompt_history.size();
            _stashed_input.clear();
            _input.clear(); _cursor = 0; _input_window_start = 0;
            echo_user(prompt);
            start_turn(prompt, prompt, /*already_echoed=*/true);
            return;
        }
        run_command_line(trimmed);
        return;
    }

    _prompt_history.push_back(line);
    _history_index = _prompt_history.size();
    _stashed_input.clear();

    if ( _turn_running ) {
        // A turn is in flight — queue this one to auto-send when it finishes.
        enqueue_pending(line, PendingKind::Message);
        { std::lock_guard<std::mutex> lk(_mx); _auto_since_user = 0; } // real user input resets the auto-resume guard
        _input.clear();
        _cursor = 0;
        _input_window_start = 0;
        for ( auto& p : _pastes ) _sent_pastes.push_back(p); // keep for /paste <n>
        _pastes.clear();
        draw_live();
        return;
    }

    {
        std::lock_guard<std::mutex> lk(_mx);
        _auto_since_user = 0; // real user input resets the auto-resume guard
    }
    // The input was consumed into `line`; clear it here (start_turn leaves the
    // input alone so queued turns can't wipe in-progress typing).
    _input.clear();
    _cursor = 0;
    _input_window_start = 0;
    // Echo (which expands paste placeholders into framed preview blocks) MUST run
    // while _pastes still holds the entries — echo first, then hand them to
    // _sent_pastes and clear, then start the turn without re-echoing.
    echo_user(display);
    for ( auto& p : _pastes ) _sent_pastes.push_back(p); // keep for /paste <n>
    _pastes.clear();
    start_turn(line, display, /*already_echoed=*/true);
}

void InlineRepl::start_turn(const std::string& line, const std::string& display, bool already_echoed) {
    // NOTE: deliberately does NOT touch _input/_pastes. A turn can start from the
    // pending queue (or a workflow auto-resume) while the user is mid-sentence —
    // wiping the input here silently discarded that typing. The submit path
    // (on_enter) clears its own consumed input before calling this.
    if ( !already_echoed )
        echo_user(display);
    begin_reply();

    {
        std::lock_guard<std::mutex> lk(_mx);
        while ( !_out_chunks.empty()) _out_chunks.pop();
        _turn_done = false;
        _turn_reply.clear();
        _turn_streamed = false;
        _activity.clear();
    }
    _turn_running = true;
    _spin = 0;
    _turn_start = std::chrono::steady_clock::now();
    _turn_tool_count.store(0, std::memory_order_relaxed);
    _confirm_belled = false;

    agent::turn_active.store(true, std::memory_order_relaxed);
    agent::turn_abort.store(false, std::memory_order_relaxed);

    _worker = std::thread([this, line]() {
        std::string reply = _callback(
            line,
            // ── Stream ingress ────────────────────────────────────────────────
            // The ONLY place raw reply text enters the inline REPL. Everything
            // downstream (route_stream_chunk and the two drain loops) relies on
            // an invariant established here:
            //
            //   Chunks arriving on this callback contain only COMPLETE UTF-8
            //   code points — never a multi-byte character split in half.
            //
            // The guarantee comes from upstream, not from this file: _callback is
            // Repl::process_turn (wired in repl.cpp), which pushes every provider
            // chunk through a stateful StreamTextSanitizer. That class buffers a
            // trailing partial sequence until the continuation bytes arrive, so a
            // character split across two HTTP chunks is reassembled before it ever
            // reaches us. cURL splits on arbitrary byte boundaries, so without
            // that pass this queue WOULD receive half characters.
            //
            // Consequence: the consumers may use the cheap, stateless
            // sanitize_stream_text(), which strips terminal control bytes but is
            // byte-preserving above 0x20 (it deliberately does not validate UTF-8;
            // see the note on that function). Two rules follow, and both are
            // pinned by tests in test_suite.cpp:
            //
            //   1. Do not add UTF-8 validation to sanitize_stream_text() — it
            //      would drop the very continuation bytes this design keeps.
            //   2. If a NEW producer is ever wired to this callback, it must do
            //      its own StreamTextSanitizer pass first, or feed raw chunks
            //      through one here before pushing.
            [this](const std::string& chunk) {
                std::lock_guard<std::mutex> lk(_mx);
                _out_chunks.push(chunk);
                _turn_streamed = true;
            },
            &agent::turn_abort);
        std::lock_guard<std::mutex> lk(_mx);
        _turn_reply = reply;
        _turn_done = true;
        agent::turn_active.store(false, std::memory_order_relaxed);
        _cv.notify_all();
    });

    draw_live();
}

void InlineRepl::start_async_command(const std::string& cmd, const std::string& activity,
                                     const std::string& echo_label) {
    // A slow slash command (e.g. /compact) runs on the worker thread so the UI
    // keeps animating a spinner instead of freezing during the LLM call. The
    // worker runs `cmd`; the transcript echoes `echo_label` (defaulting to `cmd`),
    // so an automatic invocation can be labelled differently from a typed one.
    {
        std::lock_guard<std::mutex> lk(_mx);
        while ( !_out_chunks.empty()) _out_chunks.pop();
        _turn_done = false;
        _turn_reply.clear();
        _turn_streamed = false;
        _activity = activity;
    }
    _turn_running = true;
    _async_command = true;
    _async_cmd_line = echo_label.empty() ? cmd : echo_label;
    _spin = 0;
    _turn_start = std::chrono::steady_clock::now();

    agent::turn_active.store(true, std::memory_order_relaxed);
    agent::turn_abort.store(false, std::memory_order_relaxed);

    _worker = std::thread([this, cmd]() {
        std::string result = _command_cb ? _command_cb(cmd) : ("unknown command: " + cmd);
        std::lock_guard<std::mutex> lk(_mx);
        _turn_reply = result;
        _turn_done = true;
        agent::turn_active.store(false, std::memory_order_relaxed);
        _cv.notify_all();
    });

    draw_live();
}

void InlineRepl::finish_async_command() {
    if ( _worker.joinable())
        _worker.join();
    std::string result;
    {
        std::lock_guard<std::mutex> lk(_mx);
        result = _turn_reply;
        _activity.clear();
    }
    erase_live();
    _turn_running = false;
    _async_command = false;
    agent::turn_active.store(false, std::memory_order_relaxed);
    render_command(_async_cmd_line, result);
    bool has_pending;
    {
        std::lock_guard<std::mutex> lk(_mx);
        has_pending = !_pending.empty();
    }
    if ( has_pending )
        drain_pending();
    else
        draw_live();
}

void InlineRepl::poll_worker() {
    if ( !_turn_running )
        return;
    // A modal menu / ask dialog owns the screen — don't let streamed output draw
    // over it. The chunks stay buffered and flush when it closes.
    if ( _in_settings || _in_list || _asking )
        return;

    std::vector<std::string> chunks;
    bool done = false;
    bool need_confirm = false;
    bool need_ask = false;
    tools::ConfirmRequest req;
    {
        std::lock_guard<std::mutex> lk(_mx);
        while ( !_out_chunks.empty()) {
            chunks.push_back(_out_chunks.front());
            _out_chunks.pop();
        }
        done = _turn_done;
        if ( _confirm_pending && !_confirming ) {
            need_confirm = true;
            req = _confirm_req;
        }
        if ( _ask_pending && !_asking )
            need_ask = true;
    }

    // Flush any streamed output we just drained BEFORE showing a confirm dialog —
    // otherwise those chunks (the model's text leading up to the tool call) are
    // silently dropped when a confirm request arrives in the same poll.
    if ( !chunks.empty()) {
        erase_live();
        bool collapse = _config.thinking_collapse && _config.thinking_stream;
        for ( const auto& c : chunks ) {
            if ( collapse )
                route_stream_chunk(c);
            else
                _line_buf += sanitize_stream_text(c);
        }
        flush_lines();
        draw_live();
    }

    if ( need_confirm ) {
        render_confirm_dialog(req);
        _confirming = true;
        return;
    }

    if ( need_ask ) {
        render_ask_dialog();
        _asking = true;
        return;
    }

    if ( done && !_confirming && !_asking ) {
        if ( _async_command )
            finish_async_command();
        else
            finish_turn();
    }
}

void InlineRepl::finish_turn() {
    if ( _worker.joinable())
        _worker.join();

    std::string reply;
    bool streamed;
    {
        std::lock_guard<std::mutex> lk(_mx);
        bool collapse = _config.thinking_collapse && _config.thinking_stream;
        while ( !_out_chunks.empty()) {
            if ( collapse )
                route_stream_chunk(_out_chunks.front());
            else
                _line_buf += sanitize_stream_text(_out_chunks.front());
            _out_chunks.pop();
        }
        reply = _turn_reply;
        streamed = _turn_streamed;
        _activity.clear();
    }

    // The transient reasoning preview lived in the live block; erasing it here is
    // exactly the collapse — only the answer below remains in the transcript.
    erase_live();
    _think_preview.clear();
    _stream_in_think = false;
    // The tail of the answer, to tell a question from a statement for the bell.
    std::string answer_tail = streamed ? _line_buf : reply;
    if ( !streamed && !reply.empty())
        _line_buf += reply;
    flush_lines();
    if ( !_line_buf.empty()) {
        emit_reply_line(_line_buf);
        _line_buf.clear();
    }
    // Trailing blank lines are intentionally not flushed. The next message's
    // leading blank line provides the separation from the upcoming prompt.
    if ( _in_code ) {
        _in_code = false;
        _code_lang = Language::none;
    }
    _turn_running = false;
    _in_reply = false;

    // Attention cue: a turn that ran long enough that you may have looked away
    // gets a one-line digest with a bell — what happened while you were gone.
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - _turn_start).count();
    int tools = _turn_tool_count.load(std::memory_order_relaxed);
    if ( secs >= 8 ) {
        std::string d = "done in " + std::to_string(secs) + "s";
        if ( tools > 0 )
            d += " · " + std::to_string(tools) + ( tools == 1 ? " tool call" : " tool calls" );
        wr("\n" + _theme.accent + "● " + d + Theme::reset + "\n"); // digest line (bell is separate)
    }

    // Terminal bell on a finished answer, gated by the `bell` setting: `always`
    // rings on every answer; `question`/`attention` ring only when the answer is a
    // question; `never` stays silent. (Tool-permission and workflow bells below.)
    {
        std::string tail = answer_tail;
        while ( !tail.empty() && std::isspace(static_cast<unsigned char>(tail.back())))
            tail.pop_back();
        bool is_question = !tail.empty() && tail.back() == '?';
        int need = is_question ? 2 /*question*/ : 4 /*always*/;
        if ( bell_level(_config.bell) >= need )
            wr("\a");
    }

    // Warn once when the session crosses 80% / 100% of a configured cost/token budget.
    std::string warn = budget_warning();
    if ( !warn.empty())
        wr("\n" + _theme.warn + "⚠ " + warn + Theme::reset + "\n");

    // Warn when the data dir's disk is nearly full — a full disk breaks saving
    // the conversation, so this must surface BEFORE it happens (once per level,
    // so it never becomes spam).
    std::string dwarn = disk_space_warning();
    if ( !dwarn.empty())
        wr("\n" + _theme.warn + "⚠ " + dwarn + Theme::reset + "\n");

    // If the context is now near its budget, summarise it before anything else
    // (so queued messages run against the smaller history). The async compaction
    // drains the pending queue itself when it finishes.
    if ( maybe_auto_compact())
        return;

    // Run whatever was queued while the turn was in flight.
    drain_pending();
}

std::string InlineRepl::disk_space_warning() {
    // Free space on the filesystem holding the data dir (conversations, logs,
    // credentials). Two thresholds, each warned ONCE per session (re-armed if
    // space is freed back above the soft level): low enough to matter, quiet
    // enough not to be spam.
    struct statvfs vfs;
    if ( statvfs(_config.home_dir.c_str(), &vfs) != 0 )
        return "";
    unsigned long long free_bytes =
        static_cast<unsigned long long>(vfs.f_bavail) * vfs.f_frsize;
    constexpr unsigned long long HARD = 50ULL * 1024 * 1024;   // saving is at risk
    constexpr unsigned long long SOFT = 200ULL * 1024 * 1024;  // heads-up
    int level = free_bytes < HARD ? 2 : ( free_bytes < SOFT ? 1 : 0 );
    if ( level == 0 ) {
        _disk_notified = 0; // recovered — re-arm the warnings
        return "";
    }
    if ( level <= _disk_notified )
        return "";
    _disk_notified = level;
    std::string mb = std::to_string(free_bytes / ( 1024 * 1024 )) + " MB free";
    if ( level == 2 )
        return "disk space critically low (" + mb + ") — saving the conversation may fail; "
               "free some space (old sessions: /sessions)";
    return "disk space is getting low (" + mb + ") — consider pruning old sessions (/sessions) or logs";
}

std::string InlineRepl::budget_warning() {
    long in = _stats.session_input.load(std::memory_order_relaxed);
    long out = _stats.session_output.load(std::memory_order_relaxed);
    long total = in + out;

    double frac = 0.0;
    std::string detail;
    if ( _config.budget_usd > 0.0 ) {
        double cost = _config.session_cost(in, out,
                                           _stats.session_cached.load(std::memory_order_relaxed),
                                           _stats.session_cache_creation.load(std::memory_order_relaxed));
        if ( cost >= 0 ) {
            double f = cost / _config.budget_usd;
            if ( f > frac ) {
                frac = f;
                char b[96];
                std::snprintf(b, sizeof(b), "$%.4f of $%.2f budget", cost, _config.budget_usd);
                detail = b;
            }
        }
    }
    if ( _config.budget_tokens > 0 ) {
        double f = static_cast<double>(total) / static_cast<double>(_config.budget_tokens);
        if ( f > frac ) {
            frac = f;
            detail = std::to_string(total) + " of " + std::to_string(_config.budget_tokens) + " token budget";
        }
    }

    int level = frac >= 1.0 ? 100 : ( frac >= 0.8 ? 80 : 0 );
    if ( level <= _budget_notified )
        return "";
    _budget_notified = level;
    if ( level >= 100 )
        return "session budget reached: " + detail;
    return "approaching session budget: " + detail +
           " (" + std::to_string(static_cast<int>(frac * 100)) + "%)";
}

bool InlineRepl::maybe_auto_compact() {
    if ( !_config.auto_compact )
        return false;
    size_t budget = _config.compaction_budget();
    if ( budget == 0 ) {
        if ( _config.auto_compact_max_tokens > 0 )
            budget = _config.auto_compact_max_tokens;
        else
            return false; // unknown model window — nothing reliable to measure against
    }
    long ctx = _stats.context_tokens.load(std::memory_order_relaxed);
    if ( ctx <= 0 )
        ctx = static_cast<long>(_conversation.estimate_tokens(_config.provider));
    if ( ctx <= 0 )
        return false; // no usage reported yet
    size_t pct = ( _config.auto_compact_pct >= 10 && _config.auto_compact_pct <= 100 )
               ? _config.auto_compact_pct : 80;
    size_t target = budget * pct / 100;
    if ( _config.auto_compact_max_tokens > 0 && target > _config.auto_compact_max_tokens )
        target = _config.auto_compact_max_tokens;
    if ( static_cast<size_t>(ctx) < target )
        return false;
    // Need at least a couple of exchanges to be worth summarising; compact_history
    // itself declines a near-empty history, but avoid the spinner flash for it.
    int non_system = 0;
    for ( const auto& m : _conversation.messages())
        if ( m.role != Role::SYSTEM ) ++non_system;
    if ( non_system < 4 )
        return false;

    int used = static_cast<int>(static_cast<long long>(ctx) * 100 / static_cast<long long>(target));
    start_async_command("/compact", "auto-compacting",
                        "auto-compact (context " + std::to_string(used) + "% of trigger limit)");
    return true;
}

// The confirm menu is one source of truth: (label, decision) pairs, index 0
// always the safe Deny so the default selection can never approve anything.
static std::vector<std::pair<std::string, tools::Decision>>
confirm_choices(const tools::ConfirmRequest& req) {
    std::vector<std::pair<std::string, tools::Decision>> c = {
        { "Deny", tools::Decision::deny },
        { "Deny with a reason", tools::Decision::deny },
        { "Allow once", tools::Decision::once },
        { "Allow for the rest of this turn", tools::Decision::turn },
        { "Allow for this session", tools::Decision::session },
    };
    if ( req.can_similar )
        c.push_back({ "Allow all `" + req.similar_key + "`", tools::Decision::similar });
    return c;
}

static std::vector<std::string> confirm_options(const tools::ConfirmRequest& req) {
    std::vector<std::string> opts;
    for ( const auto& c : confirm_choices(req))
        opts.push_back(c.first);
    return opts;
}

void InlineRepl::draw_confirm_menu(const tools::ConfirmRequest& req, bool redraw) {
    std::string out;
    if ( redraw && _confirm_menu_lines > 0 )
        out += "\r\033[" + std::to_string(_confirm_menu_lines) + "A"; // back up to the top
    out += "\033[J"; // clear the region

    if ( _confirm_note_mode ) {
        // A single input line for the deny reason.
        out += _theme.dim + "reason (Enter to deny, Esc to cancel):" + Theme::reset + "\r\n";
        out += "\033[1;7m ❯ \033[0m " + _confirm_note_buf;
        out += "\r\n";
        wr(out);
        _confirm_menu_lines = 2;
        return;
    }

    std::vector<std::string> opts = confirm_options(req);
    int n = static_cast<int>(opts.size());
    for ( int i = 0; i < n; ++i ) {
        if ( i == _confirm_selection )
            out += "\033[1;7m ❯ " + opts[i] + " \033[0m";  // highlighted (reverse video)
        else
            out += _theme.dim + "   " + opts[i] + Theme::reset;
        out += "\r\n";
    }
    wr(out);
    _confirm_menu_lines = n;
}

// /queue — list the prompts/commands waiting behind the running turn;
// /queue drop <n|all> removes entries. Runs immediately, even mid-turn.
void InlineRepl::queue_command(const std::string& line) {
    std::istringstream iss(line);
    std::string cmd, sub, arg;
    iss >> cmd >> sub >> arg;
    std::string edit_text;
    if ( common::to_lower(sub) == "edit" ) {
        std::getline(iss, edit_text);
        edit_text = common::trim_ws(edit_text);
    }

    // Bare /queue opens a scrollable menu of the pending items with a drop
    // action; each drop refreshes the list in place.
    if ( sub.empty()) {
        std::vector<std::string> rows, keys;
        size_t msg_count = 0, btw_count = 0, shell_count = 0, cmd_count = 0, item_count = 0;
        {
            std::lock_guard<std::mutex> lk(_mx);
            item_count = _live_updates.size() + _pending.size();
            for ( size_t i = 0; i < _live_updates.size(); ++i ) {
                if ( i == 0 ) { rows.push_back("── live updates ──"); keys.push_back(""); }
                ++btw_count;
                rows.push_back("L" + std::to_string(i + 1) + ".  live note  ·  " + queue_preview(_live_updates[i], 92));
                keys.push_back("live:" + std::to_string(i + 1));
            }
            for ( size_t i = 0; i < _pending.size(); ++i ) {
                if ( i == 0 ) { rows.push_back("── pending work ──"); keys.push_back(""); }
                switch ( _pending[i].kind ) {
                case PendingKind::Message: ++msg_count; break;
                case PendingKind::LiveNote: ++btw_count; break;
                case PendingKind::Shell: ++shell_count; break;
                case PendingKind::Command: ++cmd_count; break;
                }
                std::string kind = pending_kind_label(_pending[i].kind);
                std::string p = queue_preview(_pending[i].text, 92);
                rows.push_back("#" + std::to_string(i + 1) + ".  " + kind + "  ·  " + p);
                keys.push_back("pending:" + std::to_string(i + 1));
            }
        }
        if ( rows.empty()) {
            render_command(line, "queue is empty");
            return;
        }
        auto counted = [](size_t n, const char* singular, const char* plural) {
            return std::to_string(n) + " " + ( n == 1 ? singular : plural );
        };
        ListMenu m;
        m.title = "queue · " + counted(item_count, "item", "items") +
                  " (" + counted(msg_count, "message", "messages") +
                  ", " + counted(btw_count, "btw", "btws") +
                  ", " + counted(cmd_count, "command", "commands") +
                  ", " + counted(shell_count, "shell", "shells") + ")";
        m.rows = std::move(rows);
        m.keys = std::move(keys);
        m.actions.push_back({ 'p', "/queue promote ", "promote" });
        m.actions.push_back({ 'd', "/queue drop ", "drop" });
        m.reopen_cmd = "/queue";
        std::string hint = "↑↓ move · p promote · d drop · esc close";
        if ( btw_count > 0 )
            hint += " · btw waits for next checkpoint";
        if ( cmd_count + shell_count > 0 )
            hint += " · commands/shells run when idle";
        m.hint = hint;
        if ( btw_count == 0 && msg_count == 0 )
            m.hint += " · no messages yet";
        open_list_menu(std::move(m));
        return;
    }

    std::string result;
    {
        std::lock_guard<std::mutex> lk(_mx);
        if ( common::to_lower(sub) == "promote" ) {
            if ( arg.rfind("pending:", 0) == 0 ) arg = arg.substr(8);
            int n = 0;
            try { n = std::stoi(arg); } catch ( ... ) { n = 0; }
            if ( n < 1 || n > static_cast<int>(_pending.size()))
                result = "no queued item #" + arg + " (see /queue)";
            else if ( _pending[n - 1].kind != PendingKind::Message )
                result = "only message items can be promoted";
            else {
                PendingItem item = std::move(_pending[n - 1]);
                _pending.erase(_pending.begin() + ( n - 1 ));
                _pending.push_front(std::move(item));
                result = "promoted queued item #" + std::to_string(n);
            }
        } else if ( common::to_lower(sub) == "edit" ) {
            if ( edit_text.empty()) {
                result = "usage: /queue edit <n|live:n> <text>";
            } else if ( arg.rfind("live:", 0) == 0 ) {
                if ( arg.rfind("pending:", 0) == 0 ) arg = arg.substr(8);
                int n = 0;
                try { n = std::stoi(arg.substr(5)); } catch ( ... ) { n = 0; }
                if ( n < 1 || n > static_cast<int>(_live_updates.size()))
                    result = "no live note #" + arg.substr(5) + " (see /queue)";
                else {
                    _live_updates[n - 1] = edit_text;
                    result = "updated live note #" + std::to_string(n);
                }
            } else {
                int n = 0;
                try { n = std::stoi(arg); } catch ( ... ) { n = 0; }
                if ( n < 1 || n > static_cast<int>(_pending.size()))
                    result = "no queued item #" + arg + " (see /queue)";
                else {
                    _pending[n - 1].text = edit_text;
                    result = "updated queued item #" + std::to_string(n);
                }
            }
        } else if ( common::to_lower(sub) == "drop" ) {
            auto count_kind = [](InlineRepl::PendingKind k) {
                switch ( k ) {
                case InlineRepl::PendingKind::Message: return "message";
                case InlineRepl::PendingKind::LiveNote: return "btw";
                case InlineRepl::PendingKind::Shell: return "shell";
                case InlineRepl::PendingKind::Command: return "command";
                }
                return "item";
            };
            if ( common::to_lower(arg) == "all" ) {
                size_t n = _pending.size() + _live_updates.size(), msg = 0, btw = _live_updates.size(), shell = 0, cmd = 0;
                for ( const auto& p : _pending ) {
                    switch ( p.kind ) {
                    case PendingKind::Message: ++msg; break;
                    case PendingKind::LiveNote: ++btw; break;
                    case PendingKind::Shell: ++shell; break;
                    case PendingKind::Command: ++cmd; break;
                    }
                }
                _pending.clear();
                _live_updates.clear();
                result = "dropped all queued items";
                if ( n > 0 ) {
                    result += " (" + std::to_string(n) + " total: " +
                              std::to_string(msg) + " message(s), " +
                              std::to_string(btw) + " btw, " +
                              std::to_string(cmd) + " command(s), " +
                              std::to_string(shell) + " shell(s))";
                }
                if ( btw > 0 )
                    result += " · btw updates wait for the next checkpoint";
            } else {
                if ( arg.rfind("live:", 0) == 0 ) {
                    int n = 0;
                    try { n = std::stoi(arg.substr(5)); } catch ( ... ) { n = 0; }
                    if ( n < 1 || n > static_cast<int>(_live_updates.size()) )
                        result = _live_updates.empty() ? "queue is empty" : "no live note #" + arg.substr(5) + " (see /queue)";
                    else {
                        std::string dropped = queue_preview(_live_updates[n - 1], 60);
                        _live_updates.erase(_live_updates.begin() + ( n - 1 ));
                        result = "dropped live note #" + std::to_string(n) + ": " + dropped;
                    }
                    render_command(line, result);
                    return;
                }
                if ( arg.rfind("pending:", 0) == 0 ) arg = arg.substr(8);
                int n = 0;
                try { n = std::stoi(arg); } catch ( ... ) { n = 0; }
                if ( n < 1 || n > static_cast<int>(_pending.size()))
                    result = _pending.empty() ? "queue is empty"
                                              : "no queued item #" + arg + " (see /queue)";
                else {
                    std::string kind = count_kind(_pending[n - 1].kind);
                    std::string dropped = queue_preview(_pending[n - 1].text, 60);
                    _pending.erase(_pending.begin() + ( n - 1 ));
                    result = "dropped #" + arg + " [" + kind + "]: " + dropped;
                }
            }
        } else {
            result = "usage: /queue [drop <n|all>|edit <n|live:n> <text>]";
        }
    }
    render_command(line, result);
}

void InlineRepl::render_command(const std::string& cmd, const std::string& result) {
    erase_live();
    // A system message: the command echoed with a ⚙ marker, a blank line, then
    // its result (the blank separates the header from the content for clarity).
    wr("\n" + _theme.command + "⚙ " + Theme::reset + cmd + "\n\n");
    int width = term_cols() - 4;
    if ( width < 8 ) width = 8;
    std::istringstream ls(result);
    std::string line;
    while ( std::getline(ls, line)) {
        line = sanitize_control(line);
        for ( const auto& seg : word_wrap(line, width))
            wr("  " + seg + "\n");
    }
    // No trailing blank: the live block's own leading spacer separates the result
    // from the prompt (and from a following command), so adding one here would
    // double the gap when commands are chained.
    draw_live();
}

void InlineRepl::render_confirm_dialog(const tools::ConfirmRequest& req) {
    erase_live();

    // Attention cue. render_confirm_dialog runs exactly once per dialog (redraws go
    // through draw_confirm_menu), so this rings at most once per confirmation.
    auto ran = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - _turn_start).count();
    if ( !req.danger.empty()) {
        // A DANGEROUS (danger-listed) command always rings at any non-silent bell
        // level, immediately — this is exactly the prompt you must not miss, even if
        // you're watching. (Independent of the once-per-turn attention throttle.)
        if ( bell_level(_config.bell) >= 1 )
            wr("\a");
    } else if ( ran >= 4 && !_confirm_belled && bell_level(_config.bell) >= 3 ) {
        // A normal tool-permission prompt is an "attention" event: rung once per
        // turn, and only after the turn ran unattended for a few seconds.
        wr("\a");
        _confirm_belled = true;
    }

    // Discard anything typed before the prompt appeared so type-ahead can never
    // select an option — the whole point is that a stray keypress cannot approve.
    tcflush(STDIN_FILENO, TCIFLUSH);
    wr("\033[?25l"); // hide the cursor while the menu is up

    if ( !req.danger.empty())
        wr("\n" + _theme.danger + "⚠ dangerous command — review carefully: " + req.danger + Theme::reset + "\n");
    wr("\n" + _theme.warn + "? " + req.tool + " wants to run:" + Theme::reset + "\n");
    wr(_theme.warn + req.summary + Theme::reset + "\n");

    // Diff preview (write_file / edit_file): colour -/+ lines like a diff.
    if ( !req.preview.empty()) {
        wr("\n");
        std::istringstream ps(req.preview);
        std::string line;
        while ( std::getline(ps, line)) {
            std::string colour = _theme.dim;
            if ( !line.empty() && line[0] == '+' ) colour = _theme.command;
            else if ( !line.empty() && line[0] == '-' ) colour = _theme.danger;
            wr(colour + line + Theme::reset + "\n");
        }
    }
    wr("\n" + _theme.dim + "Select with ↑/↓ and press Enter (Esc denies). Letters do nothing." + Theme::reset + "\n");
    wr(_theme.dim + "Choices are recorded in the transcript; dangerous commands always ring." + Theme::reset + "\n");

    _confirm_selection = 0;   // Deny
    _confirm_menu_lines = 0;
    draw_confirm_menu(req, false);
}

void InlineRepl::commit_confirm(tools::Decision d, const std::string& label) {
    // Erase the menu and record the choice in the transcript.
    if ( _confirm_menu_lines > 0 )
        wr("\r\033[" + std::to_string(_confirm_menu_lines) + "A\033[J");
    wr("\033[?25h"); // restore the cursor
    wr("\033[1m→ " + label + "\033[0m\n");
    _confirm_menu_lines = 0;

    {
        std::lock_guard<std::mutex> lk(_mx);
        _confirm_decision = d;
        _confirm_note = _confirm_note_buf;
        _confirm_answered = true;
        _confirm_pending = false;
    }
    _cv.notify_all();
    _confirming = false;
    _confirm_note_mode = false;
    _confirm_note_buf.clear();
    draw_live();
}

void InlineRepl::shell_command() {
    // Design (recovered from the planning session): /shell is a FULL handover —
    // no split screen. It runs locally and immediately (jumps the queue), but
    // never mid-turn: a streaming reply and an interactive shell can't share
    // the terminal, so mid-turn it only prints a "try again" notice.
    if ( _turn_running || _confirming || _asking ) {
        erase_live();
        wr("\n" + _theme.dim +
           "⚙ /shell — not available while the AI is answering. Wait for the reply to finish and try again." +
           Theme::reset + "\n");
        draw_live();
        return;
    }

    erase_live();
    wr("\n" + _theme.dim + "── shell — type `exit` to return to the agent ──" + Theme::reset + "\n");
    // Hand the tty over: restore canonical mode for the child, run an
    // interactive shell, then re-enter raw mode. On return: redraw ONLY —
    // never the clear-screen sequence, or the transcript/scrollback is lost.
    teardown();
    const char* sh = std::getenv("SHELL");
    std::string shell = ( sh && *sh ) ? sh : "/bin/sh";
    int rc = std::system(shell.c_str());
    setup();
    std::string note = "── back in the agent";
    if ( rc != 0 )
        note += " (shell exit " + std::to_string(rc) + ")";
    note += " ──";
    wr(_theme.dim + note + Theme::reset + "\n");
    draw_live();
}

// ── command execution / queue ───────────────────────────────────────────

void InlineRepl::run_command_line(const std::string& trimmed) {
    // Always called when idle (no turn running) — except the commands marked
    // "immediate" (see command_runs_immediately), which may arrive mid-turn.
    if ( trimmed == "/shell" ) {
        shell_command();
        return;
    }
    if ( trimmed == "/sessions" || trimmed.rfind("/sessions ", 0) == 0 ) {
        // Bare /sessions opens a menu of saved sessions (size + age) with a
        // delete action, so old history can be pruned to reclaim disk space.
        // The sub-forms (delete …) are plain commands handled by the Repl.
        std::string text = _command_cb ? _command_cb(trimmed) : "";
        if ( trimmed != "/sessions" || text.rfind("no saved", 0) == 0 ) {
            render_command(trimmed, text);
            return;
        }
        ListMenu m;
        m.title = "sessions";
        std::istringstream is(text);
        std::string ln;
        while ( std::getline(is, ln)) {
            if ( common::trim_ws(ln).empty())
                continue;
            // Each row arrives as "<key>|<display>"; the key drives the actions.
            size_t bar = ln.find('|');
            if ( bar == std::string::npos ) {
                m.rows.push_back(ln);
                m.keys.push_back("");
            } else {
                m.keys.push_back(ln.substr(0, bar));
                m.rows.push_back(ln.substr(bar + 1));
            }
        }
        if ( m.rows.empty()) {
            render_command(trimmed, text);
            return;
        }
        m.actions.push_back({ 'd', "/sessions delete ", "delete" });
        m.reopen_cmd = "/sessions";
        open_list_menu(std::move(m));
        return;
    }
    if ( !trimmed.empty() && trimmed[0] == '!' ) {
        // !shell passthrough: run it off-thread (it may be a build) with the
        // spinner; the Repl side records the output for the model too.
        std::string sh = common::trim_ws(trimmed.substr(1));
        if ( sh.empty()) {
            render_command(trimmed, "usage: !<shell command>   (runs locally; the model sees the output)");
            return;
        }
        start_async_command(trimmed, "running: " + sh);
        return;
    }
    if ( trimmed == "/retry" ) {
        std::string prompt = _command_cb ? _command_cb("/retry") : "nothing to retry";
        if ( prompt.empty() || prompt == "nothing to retry" )
            render_command(trimmed, "nothing to retry");
        else
            start_turn(prompt, prompt);
        return;
    }
    if ( trimmed == "/settings" ) {
        open_settings_menu();
        return;
    }
    if ( trimmed == "/context" ) {
        render_context();
        return;
    }
    if ( trimmed == "/compact" || trimmed.rfind("/compact ", 0) == 0 ) {
        // Slow LLM call — run it off-thread with a spinner instead of blocking.
        start_async_command(trimmed, "compacting");
        return;
    }
    // Bare /model opens a picker of common models for the current provider (the
    // active one pre-selected and always present); /model <name> sets any model
    // directly. Curated rather than fetched: no per-provider /models endpoint
    // derivation, and typing still reaches anything not listed.
    if ( trimmed == "/model" ) {
        // Fetch the provider's available models (Ollama's local models, an
        // OpenAI/Anthropic /models listing) — the active model first, deduped.
        std::string listed = _command_cb ? _command_cb("/model --list") : "";
        std::vector<std::string> models;
        std::set<std::string> seen;
        std::istringstream is(listed);
        std::string ln;
        while ( std::getline(is, ln)) {
            ln = common::trim_ws(ln);
            if ( !ln.empty() && seen.insert(ln).second ) models.push_back(ln);
        }
        // Fall back to the curated shortlist when the provider offers no listing.
        // The list itself lives in Config so /model, the picker and the name
        // resolver all agree on what "known" means.
        if ( models.size() <= 1 )
            for ( const auto& mdl : agent::Config::known_models_for(_config.provider))
                if ( seen.insert(mdl).second ) models.push_back(mdl);
        ListMenu m;
        m.title = "model · " + _config.provider + ( models.size() > 1 ? "" : "  (type /model <name>)" );
        m.rows = models;
        m.keys = models;
        m.select_cmd = "/model ";
        m.current = _config.model;
        open_list_menu(std::move(m));
        return;
    }

    // Bare enum/toggle settings open a small picker; with an argument they apply
    // directly. Selecting applies via the setting command.
    if ( trimmed == "/effort" || trimmed == "/thinking" ) {
        ListMenu m;
        m.title = "reasoning effort";
        m.rows = { "off", "on", "low", "medium", "high", "xhigh", "max" };
        m.keys = m.rows;
        m.select_cmd = "/thinking ";
        m.current = _config.thinking.empty() ? "off" : _config.thinking;
        open_list_menu(std::move(m));
        return;
    }
    // /raw shows the exact request/response of the last turn — prose (JSON), so
    // open it straight into the scrollable, word-wrapped detail view.
    if ( trimmed == "/raw" || trimmed.rfind("/raw ", 0) == 0 ) {
        std::string text = _command_cb ? _command_cb(trimmed) : "";
        open_list_detail("raw", text);
        return;
    }

    // /jobs <id> shows a background job's output (potentially long) — detail view.
    // Bare /jobs (the list) and /jobs stop … stay inline.
    if ( trimmed.rfind("/jobs ", 0) == 0 ) {
        std::string rest = common::trim_ws(trimmed.substr(6));
        bool numeric = !rest.empty() && std::all_of(rest.begin(), rest.end(),
                                                    [](unsigned char c) { return std::isdigit(c); });
        if ( numeric ) {
            std::string text = _command_cb ? _command_cb(trimmed) : "";
            open_list_detail("job #" + rest, text);
            return;
        }
    }

    // /paste reviews the large pastes sent this session — bare opens a list
    // (Enter shows the full text), /paste <n> jumps straight to one.
    if ( trimmed == "/paste" || trimmed.rfind("/paste ", 0) == 0 ) {
        std::string arg = trimmed.size() > 6 ? common::trim_ws(trimmed.substr(6)) : "";
        if ( _sent_pastes.empty()) {
            render_command(trimmed, "no pastes sent this session");
            return;
        }
        if ( !arg.empty()) {
            int n = 0;
            try { n = std::stoi(arg); } catch ( ... ) { n = 0; }
            if ( n < 1 || n > static_cast<int>(_sent_pastes.size())) {
                render_command(trimmed, "no paste #" + arg + " (see /paste)");
                return;
            }
            open_list_detail("paste #" + arg, _sent_pastes[n - 1].content);
            return;
        }
        ListMenu m;
        m.title = "pastes this session";
        for ( size_t i = 0; i < _sent_pastes.size(); ++i ) {
            const std::string& body = _sent_pastes[i].content;
            size_t lines = static_cast<size_t>(std::count(body.begin(), body.end(), '\n')) +
                           ( body.empty() || body.back() == '\n' ? 0 : 1 );
            std::string first = body.substr(0, body.find('\n'));
            for ( char& ch : first ) if ( ch == '\t' ) ch = ' ';
            if ( first.size() > 50 ) first = first.substr(0, 50) + "…";
            m.rows.push_back("#" + std::to_string(i + 1) + " · " + std::to_string(lines) +
                             " lines · " + first);
            m.keys.push_back(std::to_string(i + 1));
            m.details.push_back(body);
        }
        open_list_menu(std::move(m));
        return;
    }

    if ( trimmed == "/bell" ) {
        ListMenu m;
        m.title = "terminal bell";
        m.rows = { "never", "ask_user", "question", "attention", "always" };
        m.keys = m.rows;
        m.select_cmd = "/bell ";
        m.current = _config.bell.empty() ? "attention" : _config.bell;
        open_list_menu(std::move(m));
        return;
    }
    if ( trimmed == "/profile" ) {
        ListMenu m;
        m.title = "tool profile";
        m.rows = {
            "full      — all registered tools enabled",
            "code      — coding tools (disables web, workflow)",
            "research  — read-only exploration + web search",
            "review    — read-only audit (no web/mutations)",
            "minimal   — read, edit, and bash only"
        };
        m.keys = { "full", "code", "research", "review", "minimal" };
        m.select_cmd = "/profile ";
        m.current = _config.tool_profile.empty() ? "full" : _config.tool_profile;
        open_list_menu(std::move(m));
        return;
    }
    if ( trimmed == "/tools list" ) {
        if ( _tools_provider ) {
            auto tools = _tools_provider();
            if ( !tools.empty()) {
                ListMenu m;
                m.title = "tools";
                for ( const auto& t : tools ) {
                    std::string glyph = t.enabled ? "✓" : "○";
                    std::string row = glyph + " " + t.name;
                    if ( row.size() < 24 ) row += std::string(24 - row.size(), ' ');
                    row += " [" + t.group + "]  ~" + std::to_string(t.schema_tokens) + " tok";
                    if ( t.mutating ) row += "  (mutating)";
                    m.rows.push_back(row);
                    m.keys.push_back(t.name);

                    std::string detail = "Tool: " + t.name + "\n";
                    detail += "Group: " + t.group + "\n";
                    detail += "Status: " + std::string(t.enabled ? "enabled" : "disabled") + "\n";
                    detail += "Mutating: " + std::string(t.mutating ? "yes" : "no") + "\n";
                    detail += "Estimated schema tokens: ~" + std::to_string(t.schema_tokens) + "\n\n";
                    detail += "Description:\n" + t.description + "\n";
                    m.details.push_back(detail);
                }
                m.hint = "↑↓ select · Enter details · esc close";
                open_list_menu(std::move(m));
                return;
            }
        }
    }
    if ( trimmed == "/mcp" ) {
        if ( _mcp_provider ) {
            auto servers = _mcp_provider();
            if ( !servers.empty()) {
                ListMenu m;
                m.title = "MCP servers";
                for ( const auto& s : servers ) {
                    std::string glyph = !s.enabled ? "○" : ( s.connected ? "✓" : "✗" );
                    std::string row = glyph + " " + s.name + "  [" + s.transport + "]";
                    if ( !s.enabled ) row += "  (disabled)";
                    else if ( !s.connected ) row += "  (" + (s.error.empty() ? "disconnected" : s.error) + ")";
                    else row += "  (" + std::to_string(s.tool_names.size()) + " tools)";
                    m.rows.push_back(row);
                    m.keys.push_back(s.name);

                    std::string detail = "MCP Server: " + s.name + "\n";
                    detail += "Transport: " + s.transport + "\n";
                    detail += "Status: " + std::string(!s.enabled ? "disabled" : (s.connected ? "connected" : "disconnected")) + "\n";
                    if ( !s.error.empty()) detail += "Error: " + s.error + "\n";
                    detail += "\nExposed tools (" + std::to_string(s.tool_names.size()) + "):\n";
                    for ( const auto& tn : s.tool_names )
                        detail += "  • " + tn + "\n";
                    m.details.push_back(detail);
                }
                m.actions.push_back({ 'e', "/mcp enable ", "enable" });
                m.actions.push_back({ 'd', "/mcp disable ", "disable" });
                m.actions.push_back({ 'r', "/mcp refresh", "refresh" });
                m.reopen_cmd = "/mcp";
                m.hint = "↑↓ select · Enter details · e enable · d disable · r refresh · esc close";
                open_list_menu(std::move(m));
                return;
            }
        }
    }
    if ( trimmed == "/tools" || trimmed == "/stream" || trimmed == "/strict" || trimmed == "/plan" ) {
        ListMenu m;
        if ( trimmed == "/tools" ) {
            m.title = "tool confirmation";
            m.rows = { "confirm", "auto", "insecure" };
            m.current = _config.insecure ? "insecure" : ( _config.confirm_tools ? "confirm" : "auto" );
        } else if ( trimmed == "/stream" ) {
            m.title = "live reasoning";
            m.rows = { "off", "on", "collapse" };
            m.current = !_config.thinking_stream ? "off" : ( _config.thinking_collapse ? "collapse" : "on" );
        } else if ( trimmed == "/strict" ) {
            m.title = "strict (confirm safe commands)";
            m.rows = { "off", "on" };
            m.current = _config.strict ? "on" : "off";
        } else { // /plan
            m.title = "plan mode (read-only)";
            m.rows = { "off", "on" };
            m.current = _config.plan_mode ? "on" : "off";
        }
        m.keys = m.rows;
        m.select_cmd = trimmed + " ";
        open_list_menu(std::move(m));
        return;
    }

    // Workflows open the multi-level drill-down menu (runs → steps → step content),
    // built from the live snapshot. `/workflows <id>` deep-links into that run's
    // steps; cancel/retry run the text command and print the result.
    if ( trimmed == "/workflows" ) { open_workflows_menu(); return; }
    if ( trimmed.rfind("/workflows ", 0) == 0 ) {
        std::string arg = common::trim_ws(trimmed.substr(11));
        bool is_num = !arg.empty();
        for ( char ch : arg ) if ( !std::isdigit(static_cast<unsigned char>(ch))) { is_num = false; break; }
        if ( is_num ) { open_workflows_menu(std::stoi(arg)); return; }
        std::string text = _command_cb ? _command_cb(trimmed) : "";
        render_command(trimmed, text);
        return;
    }

    // Reader commands open a scrollable, dismissable list menu instead of dumping
    // a long block into the transcript (a big /history used to flood it). Works for
    // the bare list and the detail form (e.g. /memories foo).
    {
        std::string base = trimmed;
        size_t sp = base.find_first_of(" \t");
        if ( sp != std::string::npos ) base = base.substr(0, sp);
        static const std::set<std::string> readers = {
            "/history", "/memories", "/roadmap", "/tasks", "/skills"
        };
        if ( readers.count(base)) {
            std::string text = _command_cb ? _command_cb(trimmed) : "";
            // The detail form (an argument: /workflows 3, /memories foo) is prose —
            // show it word-wrapped and scrollable so nothing is cut off.
            if ( trimmed != base ) {
                open_list_detail(trimmed.substr(1), text);
                return;
            }
            std::vector<std::string> all;
            std::istringstream is(text);
            std::string ln;
            while ( std::getline(is, ln))
                all.push_back(ln);

            ListMenu m;
            m.title = base.substr(1); // drop the leading '/'
            for ( const auto& row : all ) {
                std::string clean = common::trim_ws(row);
                if ( clean.empty())
                    continue;
                m.rows.push_back(row);
                if ( base == "/history" ) {
                    size_t pos = 1;
                    if ( clean.size() > 1 && clean[0] == '#' ) {
                        while ( pos < clean.size() && std::isdigit(static_cast<unsigned char>(clean[pos])) )
                            ++pos;
                    }
                    if ( clean.size() > 1 && clean[0] == '#' && pos > 1 )
                        m.keys.push_back(clean.substr(1, pos - 1));
                    else
                        m.keys.push_back("");
                }
            }
            if ( base == "/history" && m.keys.size() == m.rows.size())
                m.drill_cmd = "/history ";
            // Nothing selectable — just show the text.
            if ( m.rows.empty()) {
                render_command(trimmed, text);
                return;
            }
            open_list_menu(std::move(m));
            return;
        }
    }

    std::string result;
    if ( trimmed == "/theme" || trimmed.rfind("/theme ", 0) == 0 )
        result = apply_theme_command(trimmed); // UI-local: only touches this renderer
    else
        result = _command_cb ? _command_cb(trimmed) : ("unknown command: " + trimmed);
    render_command(trimmed, result);
}

void InlineRepl::drain_pending() {
    // Run queued items in order: a command (starts with '/') renders locally; a
    // message starts a turn (and draining stops until it finishes). CONSECUTIVE
    // messages are merged and sent as ONE turn, so a backlog typed during a long
    // answer flushes in a single round instead of one message per turn. Stops
    // early if a command starts a turn or opens the interactive menu.
    while ( true ) {
        PendingItem item;
        bool is_command = false;
        std::vector<std::string> parts; // consecutive user messages merged into one turn
        {
            std::lock_guard<std::mutex> lk(_mx);
            if ( _pending.empty())
                break;
            item = std::move(_pending.front());
            _pending.pop_front();
            is_command = item.kind != PendingKind::Message;
            if ( !is_command ) {
                parts.push_back(std::move(item.text));
                while ( !_pending.empty() && _pending.front().kind == PendingKind::Message ) {
                    parts.push_back(std::move(_pending.front().text));
                    _pending.pop_front();
                }
            }
        }
        if ( is_command ) {
            run_command_line(item.text);
            if ( _turn_running || _in_settings )
                return;
        } else {
            // The model gets the messages paragraph-separated; the transcript
            // shows each with its own "›" marker (no blank line between), so a
            // flushed backlog doesn't read as alternating speakers.
            std::string combined;
            for ( size_t i = 0; i < parts.size(); ++i )
                combined += ( i ? "\n\n" : "" ) + parts[i];
            if ( parts.size() > 1 )
                echo_user_multi(parts);
            start_turn(combined, combined, /*already_echoed=*/parts.size() > 1);
            return;
        }
    }
    draw_live();
}

// ── visual /context breakdown ───────────────────────────────────────────

void InlineRepl::render_context() {
    erase_live();

    auto fmt = [](size_t n) -> std::string {
        if ( n >= 1000 )
            return std::to_string(n / 1000) + "." + std::to_string((n % 1000) / 100) + "k";
        return std::to_string(n);
    };
    auto estimate = [this](const std::vector<Message>& msgs) {
        struct Totals { size_t system = 0, messages = 0, tool_results = 0, tool_calls = 0; size_t elided = 0; } t;
        for ( const auto& m : msgs ) {
            size_t content = Conversation::estimate_message_tokens(m, _config.provider);
            if ( m.role == Role::SYSTEM ) t.system += content;
            else if ( m.role == Role::TOOL ) {
                t.tool_results += content;
                if ( m.content.find("tool result elided") != std::string::npos ||
                     m.content.find("superseded by") != std::string::npos )
                    ++t.elided;
            } else t.messages += content;
            for ( const auto& tc : m.tool_calls )
                t.tool_calls += ( tc.arguments.size() + tc.name.size()) / 4;
        }
        return t;
    };
    auto sum = [](const auto& t) { return t.system + t.messages + t.tool_results + t.tool_calls; };
    auto raw_tool_bytes = [](const std::vector<Message>& msgs) {
        size_t n = 0;
        for ( const auto& m : msgs ) if ( m.role == Role::TOOL ) n += m.content.size();
        return n;
    };

    std::vector<Message> saved = _conversation.messages();
    std::vector<Message> effective = saved;
    if ( _config.supersede_tools )
        effective = Conversation::supersede_stale_tools(std::move(effective));
    effective = Conversation::elide_old_large_tool_results(std::move(effective));
    if ( _config.context_budget() > 0 )
        effective = _conversation.within_token_budget(_config.context_budget(), std::move(effective), _config.provider);

    size_t schema_tokens = 0;
    size_t builtin_tokens = 0, mcp_tokens = 0;
    if ( _tools_provider ) {
        for ( const auto& ti : _tools_provider() ) {
            if ( ti.enabled ) {
                schema_tokens += ti.schema_tokens;
                if ( ti.group == "mcp" ) mcp_tokens += ti.schema_tokens;
                else builtin_tokens += ti.schema_tokens;
            }
        }
    }

    auto raw = estimate(saved);
    auto eff = estimate(effective);
    size_t raw_total = sum(raw) + schema_tokens, eff_total = sum(eff) + schema_tokens;
    size_t raw_bytes = raw_tool_bytes(saved), eff_bytes = raw_tool_bytes(effective);
    size_t saved_bytes = raw_bytes > eff_bytes ? raw_bytes - eff_bytes : 0;
    size_t mem = load_memories(_config.home_dir, _config.provider).size() / 4;
    std::string proj_instr_text;
    try { proj_instr_text = load_project_instructions(std::filesystem::current_path().string()); } catch (...) {}
    size_t proj_instr_tokens = proj_instr_text.size() / 4;
    long actual = _stats.context_tokens.load(std::memory_order_relaxed);
    long last_cached = _stats.last_cached.load(std::memory_order_relaxed);
    long session_cached = _stats.session_cached.load(std::memory_order_relaxed);

    auto pct = [eff_total](size_t n) -> std::string {
        if ( eff_total == 0 ) return "0%";
        return std::to_string(static_cast<int>((100.0 * n / eff_total) + 0.5)) + "%";
    };

    wr("\n" + _theme.command + "⚙ context" + Theme::reset + "\n\n");

    const int barw = 46;
    int sysc = ( eff_total > 0 ) ? static_cast<int>(( double(eff.system) / eff_total ) * barw + 0.5) : 0;
    if ( sysc > barw ) sysc = barw;
    int msgc = barw - sysc;
    std::string blocks_sys, blocks_msg;
    for ( int i = 0; i < sysc; ++i ) blocks_sys += "█";
    for ( int i = 0; i < msgc; ++i ) blocks_msg += "█";
    wr("  " + _theme.ai + blocks_sys + _theme.command + blocks_msg + Theme::reset +
       "  " + _theme.dim + "~" + fmt(eff_total) + " effective tokens" + Theme::reset + "\n\n");

    wr("  " + _theme.ai + "●" + Theme::reset + " system prompt   " + fmt(eff.system) + "  " +
       _theme.dim + "(" + pct(eff.system) + ")" + Theme::reset + "\n");
    if ( proj_instr_tokens > 0 )
        wr("    " + _theme.dim + "└ instructions  " + fmt(proj_instr_tokens) + "  (AGENTS.md)" + Theme::reset + "\n");
    if ( mem > 0 )
        wr("    " + _theme.dim + "└ memories      " + fmt(mem) + Theme::reset + "\n");
    wr("  " + _theme.command + "●" + Theme::reset + " messages        " + fmt(eff.messages) + "  " +
       _theme.dim + "(" + pct(eff.messages) + ")" + Theme::reset + "\n");
    wr("  " + _theme.command + "●" + Theme::reset + " tool results    " + fmt(eff.tool_results) + "  " +
       _theme.dim + "(" + pct(eff.tool_results) + ")" + Theme::reset + "\n");
    if ( eff.tool_calls > 0 )
        wr("    " + _theme.dim + "└ tool calls    " + fmt(eff.tool_calls) + Theme::reset + "\n");
    if ( schema_tokens > 0 ) {
        wr("  " + _theme.command + "●" + Theme::reset + " tools schema   " + fmt(schema_tokens) + "  " +
           _theme.dim + "(" + pct(schema_tokens) + " · " + std::to_string(builtin_tokens) + " built-in, " + std::to_string(mcp_tokens) + " mcp)" + Theme::reset + "\n");
    }

    if ( raw_total != eff_total || eff.elided > 0 ) {
        wr("\n  " + _theme.dim + "effective request: " + fmt(eff_total) + " tokens; saved transcript: " +
           fmt(raw_total) + " tokens" + Theme::reset + "\n");
        if ( eff.elided > 0 || saved_bytes > 0 )
            wr("  " + _theme.dim + "elided tool results: " + std::to_string(eff.elided) +
               " · ~" + fmt(saved_bytes / 4) + " tokens avoided" + Theme::reset + "\n");
    }

    std::string limit_str;
    if ( _config.context_auto ) {
        size_t b = _config.context_budget();
        limit_str = b ? "auto (" + fmt(b) + ")" : "auto (unlimited)";
    } else {
        limit_str = _config.context_limit == 0 ? "unlimited" : fmt(_config.context_limit) + " tokens";
    }
    std::string footer = "  " + _theme.dim + "context limit: " + limit_str;
    if ( actual > 0 )
        footer += " · last turn reported " + fmt(static_cast<size_t>(actual));
    if ( last_cached > 0 )
        footer += " (" + fmt(static_cast<size_t>(last_cached)) + " cached)";
    footer += Theme::reset + std::string("\n");
    long session_creation = _stats.session_cache_creation.load(std::memory_order_relaxed);
    if ( session_cached > 0 || session_creation > 0 ) {
        int disc = _config.provider_pricing_rules(_config.provider, _config.model).discount_pct();
        footer += "  " + _theme.dim + "prompt cache:   ";
        if ( session_cached > 0 )
            footer += fmt(static_cast<size_t>(session_cached)) + " tokens cached (~" + std::to_string(disc) + "% cost reduction)";
        if ( session_creation > 0 ) {
            if ( session_cached > 0 ) footer += " · ";
            footer += fmt(static_cast<size_t>(session_creation)) + " created";
        }
        footer += std::string(Theme::reset) + "\n";
    }
    wr(footer);

    draw_live();
}

// ── interactive settings menu ───────────────────────────────────────────

void InlineRepl::open_settings_menu() {
    // Current values come from the plain text /settings output (stable
    // "key: value" lines); the theme is our own UI state.
    std::map<std::string, std::string> cur;
    if ( _command_cb ) {
        std::istringstream ss(_command_cb("/settings"));
        std::string ln;
        while ( std::getline(ss, ln)) {
            size_t colon = ln.find(':');
            if ( colon == std::string::npos )
                continue;
            cur[common::trim_ws(ln.substr(0, colon))] = common::trim_ws(ln.substr(colon + 1));
        }
    }
    auto first_word = [](const std::string& s) {
        std::istringstream is(s);
        std::string w;
        is >> w;
        return w;
    };
    std::string th = cur.count("thinking") ? cur["thinking"] : "";
    if ( th.empty() || th[0] == '(' ) th = "default";

    _settings_rows.clear();
    auto add = [&](std::string key, std::string label, std::string value, std::string group,
                   std::string desc, std::vector<std::string> opts = {}, bool number = false,
                   long lo = 0, long hi = 0, long step = 1, std::string unit = "", std::string zero = "",
                   bool tokens = false) {
        SettingRow r;
        r.key = std::move(key); r.label = std::move(label); r.value = std::move(value);
        r.group = std::move(group); r.desc = std::move(desc); r.options = std::move(opts);
        r.is_number = number; r.num_min = lo; r.num_max = hi; r.num_step = step;
        r.is_tokens = tokens;
        r.unit = std::move(unit); r.zero_label = std::move(zero);
        _settings_rows.push_back(std::move(r));
    };
    const std::string PROV = "Model & provider", TOOLS = "Tools & safety",
                      CTX = "Context", UI = "Interface";

    add("model", "model", cur.count("model") ? cur["model"] : _config.model, PROV,
        "the model this provider talks to");
    add("thinking", "reasoning", th, PROV,
        "how much the model thinks before answering",
        { "off", "on", "low", "medium", "high", "xhigh", "max" });
    add("thinking_stream", "show reasoning",
        ( !_config.thinking_stream ? "off" : ( _config.thinking_collapse ? "collapse" : "on" )), PROV,
        "stream the reasoning live (collapse hides it once the answer is done)",
        { "off", "on", "collapse" });

    add("tools", "tools", first_word(cur["tools"]), TOOLS,
        "confirm: ask before edits/commands · auto: run freely · insecure: never ask",
        { "confirm", "auto", "insecure" });
    add("strict", "strict",
        cur["tools"].find("(strict)") != std::string::npos ? "on" : "off", TOOLS,
        "in confirm mode, also confirm safe read-only shell commands", { "off", "on" });
    add("tool_call_limit", "tool budget",
        std::to_string(_config.tool_call_limit), TOOLS,
        "runaway-loop guard: pause and ask after this many tool calls in one turn "
        "(a big refactor can legitimately use hundreds)", {}, true,
        0, 2000, 50, "per turn", "unlimited");
    add("redact_secrets", "redact secrets", _config.redact_secrets ? "on" : "off", TOOLS,
        "mask credentials in tool output before it is sent to the model", { "off", "on" });
    if ( _config.provider == "claude" )
        add("advisor", "advisor", _config.advisor ? "on" : "off", TOOLS,
            "let the model consult a stronger advisor model", { "off", "on" });

    add("context", "context",
        first_word(cur.count("context") ? cur["context"] : "unlimited"), CTX,
        "INPUT budget: how much history is sent each turn. auto = 85% of the "
        "model's window (recommended) · unlimited sends everything until the API refuses");
    add("auto_compact", "auto-compact", _config.auto_compact ? "on" : "off", CTX,
        "summarise older turns when the input budget is ~80% used — needs a "
        "context budget, so it does nothing while context is unlimited", { "off", "on" });
    add("max_tokens", "max reply", std::to_string(_config.max_tokens), CTX,
        "OUTPUT cap for a SINGLE reply (not a session total); clamped to the "
        "model's own ceiling, so higher than that has no effect",
        {}, true, 1000, 200000, 4000, "", "", true);
    add("autoresume", "workflow resume", _config.workflow_autoresume ? "on" : "off", CTX,
        "when a background workflow finishes, feed its results to the model automatically",
        { "off", "on" });

    {
        // "custom" joins the cycle only when the config defines overrides, so the
        // menu never offers a palette that would look identical to its base.
        std::vector<std::string> themes = { "dark", "light", "warm", "cool", "rose" };
        if ( !_config.theme_colors.empty())
            themes.push_back("custom");
        add("theme", "theme", _theme.name, UI,
            _config.theme_colors.empty()
                ? "colour theme (never sets the terminal background) · add theme.<role> to the config for a custom one"
                : "colour theme (never sets the terminal background) · custom = base " + _config.theme_base + " + your overrides",
            themes);
    }
    add("bell", "bell", _config.bell, UI,
        "bell: always · attention (workflow/tool/?) · question · ask_user (model asks you) · never — a dangerous command always rings unless never",
        { "never", "ask_user", "question", "attention", "always" });
    add("multiline", "multiline", _config.multiline ? "on" : "off", UI,
        "wrap long/multi-line input across lines (↑↓ move between them) · "
        "Enter always sends, Ctrl-J (or Alt+Enter) inserts a newline",
        { "off", "on" });
    add("paste_preview", "paste preview",
        std::to_string(_config.paste_preview), UI,
        "lines of a large paste to echo in the transcript", {}, true, 0, 200, 1, "lines", "all");

    erase_live();
    tcflush(STDIN_FILENO, TCIFLUSH); // ignore anything typed before the menu opened
    wr("\033[?25l");                 // hide the cursor while the menu is up
    wr("\n" + _theme.command + "⚙ Settings" + Theme::reset + "   " +
       _theme.dim + "↑↓ move · ←→ change · ⏎ edit · esc close" + Theme::reset + "\n");
    _in_settings = true;
    _settings_editing = false;
    _settings_selection = 0;
    _settings_menu_lines = 0;
    draw_settings_menu(false);
}

std::string InlineRepl::setting_display_value(const SettingRow& row) const {
    if ( !row.is_number )
        return row.value;
    long v = 0;
    try { v = std::stol(row.value); } catch ( ... ) { v = row.num_min; }
    if ( v == 0 && !row.zero_label.empty())
        return row.zero_label;
    // Token budgets get thousands/millions shorthand: "64K" and "1.5M" are far
    // easier to compare at a glance than "64000" and "1500000".
    std::string s = row.is_tokens ? agent::Config::format_tokens(static_cast<size_t>(v))
                                  : std::to_string(v);
    if ( !row.unit.empty()) s += " " + row.unit;
    return s;
}

namespace { std::string clip_cells(const std::string& s, int cols); } // defined below

void InlineRepl::draw_settings_menu(bool redraw) {
    int n = static_cast<int>(_settings_rows.size());
    constexpr size_t LW = 16; // label column width (fits the longest label + a gap)
    int cols = term_cols();
    int valw = cols - static_cast<int>(LW) - 10; // room for prefix "❯ "/brackets/reset
    if ( valw < 8 ) valw = 8;

    // Build the physical lines first (group headers, rows, the selected row's help
    // line), so a scrolling viewport can window them on a short terminal without
    // miscounting the backup. Each row's VALUE is clipped to width so no line wraps.
    std::vector<std::string> phys;
    int sel_line = 0;
    std::string group;
    for ( int i = 0; i < n; ++i ) {
        const SettingRow& row = _settings_rows[i];
        if ( row.group != group ) {
            group = row.group;
            if ( !phys.empty()) phys.push_back(""); // blank spacer before a section
            phys.push_back(_theme.accent + "  " + group + Theme::reset);
        }
        std::string label = row.label;
        while ( label.size() < LW ) label += ' ';
        bool selected = ( i == _settings_selection );
        std::string val = clip_cells(setting_display_value(row), valw);
        std::string line;
        if ( selected && _settings_editing ) {
            line = "\033[1;7m ❯ " + label + clip_cells(_settings_edit_buf, valw) + "▏ \033[0m"
                 + _theme.dim + "  (⏎ apply · esc cancel)" + Theme::reset;
        } else if ( selected ) {
            std::string shown = ( row.options.empty() && !row.is_number ) ? val : "‹ " + val + " ›";
            line = "\033[1;7m ❯ " + label + shown + " \033[0m";
        } else {
            line = "   " + label + _theme.dim + val + Theme::reset;
        }
        if ( selected ) sel_line = static_cast<int>(phys.size());
        phys.push_back(line);

        if ( selected && !_settings_editing && !row.desc.empty()) {
            std::string help = row.desc;
            if ( row.is_number && !row.zero_label.empty())
                help += "  ·  0 = " + row.zero_label;
            phys.push_back(_theme.dim + "     └ " + clip_cells(help, cols - 8) + Theme::reset);
        }
    }

    // Viewport: on a terminal too short to hold the whole menu, window the lines
    // around the selection so the backup count never exceeds the physical screen.
    int vh = term_rows() - 1;
    if ( vh < 3 ) vh = 3;
    int top = 0;
    int total = static_cast<int>(phys.size());
    if ( total > vh ) {
        if ( sel_line >= vh - 1 ) top = sel_line - vh + 2;
        if ( top + vh > total ) top = total - vh;
        if ( top < 0 ) top = 0;
    }
    int end = std::min(total, top + vh);

    std::string out;
    if ( redraw && _settings_menu_lines > 0 )
        out += "\r\033[" + std::to_string(_settings_menu_lines) + "A";
    out += "\033[J";
    int lines = 0;
    for ( int i = top; i < end; ++i ) {
        out += phys[i] + "\r\n";
        lines++;
    }
    wr(out);
    _settings_menu_lines = lines;
}

void InlineRepl::adjust_number_row(int dir) {
    SettingRow& row = _settings_rows[_settings_selection];
    if ( !row.is_number )
        return;
    long v = 0;
    try { v = std::stol(row.value); } catch ( ... ) { v = row.num_min; }
    v += dir * row.num_step;
    if ( v < row.num_min ) v = row.num_min;
    if ( v > row.num_max ) v = row.num_max;
    row.value = std::to_string(v);
    if ( _command_cb )
        _command_cb("/settings " + row.key + " " + row.value);
    draw_settings_menu(true);
}

void InlineRepl::cycle_settings_row(int dir) {
    SettingRow& row = _settings_rows[_settings_selection];
    if ( row.options.empty())
        return; // free-text row, not cyclable
    int size = static_cast<int>(row.options.size());
    int idx = 0;
    for ( int i = 0; i < size; ++i )
        if ( row.options[i] == row.value ) { idx = i; break; }
    idx = ( idx + dir + size ) % size;
    row.value = row.options[idx];

    if ( row.key == "theme" )
        apply_theme_command("/theme " + row.value);
    else if ( _command_cb )
        _command_cb("/settings " + row.key + " " + row.value); // /settings delegates per key
    draw_settings_menu(true);
}

void InlineRepl::close_settings_menu() {
    if ( _settings_menu_lines > 0 )
        wr("\r\033[" + std::to_string(_settings_menu_lines) + "A\033[J");
    wr("\033[?25h"); // restore the cursor
    _settings_menu_lines = 0;
    _in_settings = false;
    _settings_editing = false;
    // Anything queued while the menu was up resumes now — but ONLY if no turn is
    // in flight. If the menu was opened mid-turn, draining here would start a
    // second turn on top of the running one (reassigning the live worker thread →
    // crash); poll_worker resumes and finishes/drains the turn after the menu closes.
    bool has_pending;
    {
        std::lock_guard<std::mutex> lk(_mx);
        has_pending = !_pending.empty();
    }
    if ( has_pending && !_turn_running )
        drain_pending();
    else
        draw_live();
}

// ── shared scrollable list / reader menu ─────────────────────────────────

namespace {
// Word-wrap a block of text into display rows of at most `width` cells, so long
// prose (workflow results, memory bodies) is fully readable by scrolling rather
// than truncated at the right edge.
std::vector<std::string> wrap_to_rows(const std::string& text, int width) {
    std::vector<std::string> rows;
    std::istringstream is(text);
    std::string line;
    while ( std::getline(is, line)) {
        std::string clean = sanitize_display(line);
        if ( clean.empty()) { rows.push_back(""); continue; }
        for ( auto& seg : word_wrap(clean, width))
            rows.push_back(seg);
    }
    return rows;
}

// Strip control chars and clip a line to `cols` display cells for the menu.
std::string clip_cells(const std::string& s, int cols) {
    std::string t = sanitize_display(s);
    auto cells = split_cells(t);
    if ( static_cast<int>(cells.size()) <= cols )
        return t;
    std::string out;
    for ( int i = 0; i < cols - 1 && i < static_cast<int>(cells.size()); ++i )
        out += cells[i];
    return out + "…";
}
} // namespace

void InlineRepl::open_list_menu(ListMenu menu) {
    erase_live();
    tcflush(STDIN_FILENO, TCIFLUSH); // no type-ahead into a just-opened menu
    wr("\033[?25l");                 // hide the cursor
    _list = std::move(menu);
    _in_list = true;
    _list_detail = false;
    _list_sel = 0;
    _list_top = 0;
    _list_lines = 0;
    // Pre-select the row matching the current value (pickers).
    if ( !_list.current.empty())
        for ( size_t i = 0; i < _list.keys.size(); ++i )
            if ( _list.keys[i] == _list.current ) { _list_sel = static_cast<int>(i); break; }
    draw_list_menu(false);
}

// Open the menu straight into a scroll-only, word-wrapped detail view — for the
// prose that reader commands with an argument return (e.g. /workflows <id>).
void InlineRepl::open_list_detail(const std::string& title, const std::string& text) {
    erase_live();
    tcflush(STDIN_FILENO, TCIFLUSH);
    wr("\033[?25l");
    _list = ListMenu{};
    _list.title = title;
    _in_list = true;
    _list_detail = true;
    _list_detail_rows = wrap_to_rows(text, term_cols() - 4);
    _list_detail_top = 0;
    _list_lines = 0;
    draw_list_menu(false);
}

int InlineRepl::menu_view_rows() const {
    // Cap the panel height so it sits at the bottom with the conversation still
    // visible above (the "overlay" feel), rather than a near-full-screen list that
    // scrolls the transcript out of view. Floor of 1 (not 3) so an absurdly short
    // terminal isn't overrun — the whole panel is title + vh + footer.
    constexpr int CAP = 15;
    int avail = term_rows() - 3; // title + a little breathing room
    int vh = avail < CAP ? avail : CAP;
    return vh < 1 ? 1 : vh;
}

void InlineRepl::draw_list_menu(bool redraw) {
    std::string out;
    if ( redraw && _list_lines > 0 )
        out += "\r\033[" + std::to_string(_list_lines) + "A";
    out += "\033[J";
    int lines = 0;
    int cols = term_cols() - 3;
    if ( cols < 8 ) cols = 8;
    int vh = menu_view_rows();
    if ( _list_detail ) {
        std::string title = clip_cells(_list.title, std::max(4, cols - 14));
        out += _theme.command + "╭─ " + title + " " + _theme.dim + "· detail" +
               _theme.command + " ─╮" + Theme::reset + "\r\n";
        lines++;
        int total = static_cast<int>(_list_detail_rows.size());
        if ( _list_detail_top > std::max(0, total - vh)) _list_detail_top = std::max(0, total - vh);
        if ( _list_detail_top < 0 ) _list_detail_top = 0;
        for ( int i = _list_detail_top; i < total && i < _list_detail_top + vh; ++i ) {
            out += "  " + clip_cells(_list_detail_rows[i], cols) + "\r\n";
            lines++;
        }
        if ( total > vh ) {
            out += _theme.dim + "  " + std::to_string(_list_detail_top + 1) + "–" +
                   std::to_string(std::min(total, _list_detail_top + vh)) + " / " +
                   std::to_string(total) + Theme::reset + "\r\n";
            lines++;
        }
        out += _theme.dim + "╰─ ↑↓ scroll · esc back ─╯" + Theme::reset + "\r\n";
        lines++;
    } else {
        std::string hint = _list.hint;
        if ( hint.empty()) {
            hint = "↑↓ move · esc close";
            if ( !_list.select_cmd.empty()) hint += " · ⏎ select";
            if ( !_list.drill_cmd.empty() || !_list.details.empty()) hint += " · ⏎ open";
            for ( const auto& a : _list.actions )
                hint += std::string(" · ") + a.key + " " + a.label;
        }
        std::string title = clip_cells(_list.title, std::max(4, cols - 14));
        out += _theme.command + "╭─ " + title + " " + _theme.dim + "· menu" +
               _theme.command + " ─╮" + Theme::reset + "\r\n";
        lines++;

        int total = static_cast<int>(_list.rows.size());
        if ( total == 0 ) {
            out += _theme.dim + "  (empty)" + Theme::reset + "\r\n";
            lines++;
        }
        if ( _list_sel < _list_top ) _list_top = _list_sel;
        if ( _list_sel >= _list_top + vh ) _list_top = _list_sel - vh + 1;
        if ( _list_top < 0 ) _list_top = 0;
        for ( int i = _list_top; i < total && i < _list_top + vh; ++i ) {
            if ( i == _list_sel )
                out += "\033[1;7m ❯ " + clip_cells(_list.rows[i], cols) + " \033[0m\r\n";
            else
                out += "   " + clip_cells(_list.rows[i], cols) + "\r\n";
            lines++;
        }
        if ( total > vh ) {
            out += _theme.dim + "  " + std::to_string(_list_sel + 1) + " / " +
                   std::to_string(total) + Theme::reset + "\r\n";
            lines++;
        }
        out += _theme.dim + "╰─ " + clip_cells(hint, std::max(4, cols - 6)) + " ─╯" + Theme::reset + "\r\n";
        lines++;
    }
    wr(out);
    _list_lines = lines;
}

void InlineRepl::handle_list_key(int c) {
    if ( _wf_active ) { handle_workflow_key(c); return; }
    int n = static_cast<int>(_list.rows.size());
    int vh = menu_view_rows();

    if ( c == 0x1b ) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv { 0, 40 * 1000 };
        if ( select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0 ) {
            int b1 = read_byte();
            if ( b1 == '[' || b1 == 'O' ) {
                int b2 = read_byte();
                if ( b2 == '5' || b2 == '6' ) read_byte(); // consume '~' of PgUp/PgDn
                if ( _list_detail ) {
                    if ( b2 == 'A' ) { _list_detail_top--; draw_list_menu(true); }
                    else if ( b2 == 'B' ) { _list_detail_top++; draw_list_menu(true); }
                    else if ( b2 == '5' ) { _list_detail_top -= vh; draw_list_menu(true); }
                    else if ( b2 == '6' ) { _list_detail_top += vh; draw_list_menu(true); }
                } else if ( n > 0 ) {
                    if ( b2 == 'A' ) { _list_sel = std::max(0, _list_sel - 1); draw_list_menu(true); }
                    else if ( b2 == 'B' ) { _list_sel = std::min(n - 1, _list_sel + 1); draw_list_menu(true); }
                    else if ( b2 == '5' ) { _list_sel = std::max(0, _list_sel - vh); draw_list_menu(true); }
                    else if ( b2 == '6' ) { _list_sel = std::min(n - 1, _list_sel + vh); draw_list_menu(true); }
                }
            }
            return;
        }
        // bare Esc: leave a detail view back to the list, or close the menu.
        if ( _list_detail ) { _list_detail = false; draw_list_menu(true); }
        else close_list_menu();
        return;
    }

    if ( _list_detail )
        return; // only scroll / esc in detail view

    // Picker: Enter applies the selected value and closes the menu.
    if (( c == '\r' || c == '\n' ) && !_list.select_cmd.empty() &&
        _list_sel < static_cast<int>(_list.keys.size()) && !_list.keys[_list_sel].empty()) {
        std::string cmd = _list.select_cmd + _list.keys[_list_sel];
        if ( _command_cb )
            _command_cb(cmd);
        close_list_menu();
        return;
    }

    if ( c == '\r' || c == '\n' ) {
        std::string detail;
        bool have = false;
        if ( !_list.drill_cmd.empty() && _list_sel < static_cast<int>(_list.keys.size()) &&
             !_list.keys[_list_sel].empty()) {
            detail = _command_cb ? _command_cb(_list.drill_cmd + _list.keys[_list_sel]) : "";
            have = true;
        } else if ( _list_sel < static_cast<int>(_list.details.size())) {
            detail = _list.details[_list_sel]; // pre-loaded content (e.g. /paste)
            have = true;
        }
        if ( have ) {
            _list_detail_rows = wrap_to_rows(detail, term_cols() - 4);
            _list_detail = true;
            _list_detail_top = 0;
            draw_list_menu(true);
            return;
        }
    }

    for ( const auto& a : _list.actions ) {
        if ( c != a.key )
            continue;
        // /queue is managed inside the renderer (its data is the pending deque);
        // everything else goes through the command callback / run_command_line.
        auto exec = [&](const std::string& cmd) {
            if ( cmd.rfind("/queue", 0) == 0 ) queue_command(cmd);
            else if ( _command_cb ) _command_cb(cmd);
        };
        if ( _list_sel < static_cast<int>(_list.keys.size()) && !_list.keys[_list_sel].empty())
            exec(a.cmd + _list.keys[_list_sel]);
        // Refresh the list in place (e.g. after a drop/cancel) by re-opening it.
        std::string reopen = _list.reopen_cmd;
        close_list_menu();
        if ( !reopen.empty()) {
            if ( reopen.rfind("/queue", 0) == 0 ) queue_command(reopen);
            else run_command_line(reopen);
        }
        return;
    }
}

namespace {
// Flatten a possibly multi-line string to one line (whitespace collapsed) and clip.
std::string wf_flat(const std::string& s, size_t max) {
    std::string o;
    bool sp = true; // drop leading whitespace
    for ( char c : s ) {
        if ( c == '\n' || c == '\t' || c == '\r' || c == ' ' ) {
            if ( !sp ) o += ' ';
            sp = true;
        } else { o += c; sp = false; }
    }
    while ( !o.empty() && o.back() == ' ' ) o.pop_back();
    if ( o.size() > max ) o = o.substr(0, max) + "…";
    return o;
}
std::string wf_glyph(const std::string& st) {
    if ( st == "done" ) return "✔";
    if ( st == "error" ) return "✗";
    if ( st == "running" ) return "▸";
    if ( st == "cancelled" ) return "∅";
    return "○"; // pending
}
} // namespace

void InlineRepl::open_workflows_menu(int run_id) {
    erase_live();
    tcflush(STDIN_FILENO, TCIFLUSH);
    wr("\033[?25l"); // hide the cursor
    _in_list = true;
    _list_detail = false;
    _wf_active = true;
    _list_lines = 0;
    _list_sel = 0;
    _list_top = 0;
    if ( run_id >= 0 ) { _wf_level = 1; _wf_run_id = run_id; }
    else { _wf_level = 0; _wf_run_id = -1; }
    build_workflow_level(false);
}

void InlineRepl::build_workflow_level(bool redraw) {
    std::vector<WorkflowRun> runs = _wf_provider ? _wf_provider() : std::vector<WorkflowRun>{};
    ListMenu m;
    if ( _wf_level == 1 ) {
        const WorkflowRun* run = nullptr;
        for ( const auto& r : runs ) if ( r.id == _wf_run_id ) { run = &r; break; }
        if ( !run ) { _wf_level = 0; _wf_run_id = -1; _list_top = 0; _list_sel = 0; } // run went away — fall back to the list (reset scroll like a normal transition)
        else {
            m.title = "workflows › #" + std::to_string(run->id) + " " + wf_flat(run->name, 40);
            m.hint = "↑↓ move · ⏎ open step · esc back";
            for ( size_t i = 0; i < run->steps.size(); ++i ) {
                const auto& st = run->steps[i];
                m.rows.push_back(wf_glyph(st.status) + " " + wf_flat(st.task, 56) + "  [" + st.status + "]");
                m.keys.push_back(std::to_string(i));
            }
        }
    }
    if ( _wf_level == 0 ) {
        m.title = "workflows";
        m.hint = "↑↓ move · ⏎ open · c cancel · r retry · esc close";
        for ( const auto& r : runs ) {
            int done = 0;
            for ( const auto& st : r.steps )
                if ( st.status == "done" || st.status == "error" ) ++done;
            size_t total = r.steps.size();
            m.rows.push_back(wf_glyph(r.status) + " " + wf_flat(r.name, 40) + "  [" +
                             std::to_string(total) + ( total == 1 ? " step]" : " steps]" ) + "  " +
                             std::to_string(done) + "/" + std::to_string(total) +
                             ( r.parallel ? "  ∥" : "" ));
            m.keys.push_back(std::to_string(r.id));
        }
        m.actions.push_back({ 'c', "", "cancel" });
        m.actions.push_back({ 'r', "", "retry" });
    }
    _list = std::move(m);
    int rows = static_cast<int>(_list.rows.size());
    if ( _list_sel >= rows ) _list_sel = rows > 0 ? rows - 1 : 0;
    if ( _list_sel < 0 ) _list_sel = 0;
    draw_list_menu(redraw);
}

void InlineRepl::workflow_enter() {
    if ( _wf_level == 0 ) {
        if ( _list_sel >= static_cast<int>(_list.keys.size()) || _list.keys[_list_sel].empty())
            return;
        try { _wf_run_id = std::stoi(_list.keys[_list_sel]); } catch ( ... ) { return; }
        _wf_level = 1;
        _list_sel = 0;
        _list_top = 0;
        build_workflow_level(true);
        return;
    }
    // Level 1: open the selected step's content in the scrollable detail view.
    std::vector<WorkflowRun> runs = _wf_provider ? _wf_provider() : std::vector<WorkflowRun>{};
    const WorkflowRun* run = nullptr;
    for ( const auto& r : runs ) if ( r.id == _wf_run_id ) { run = &r; break; }
    if ( !run || _list_sel >= static_cast<int>(run->steps.size()))
        return;
    const auto& st = run->steps[_list_sel];
    std::string body = "task:\n" + st.task + "\n\nstatus: " + st.status + "\n\n" +
                       ( st.result.empty() ? "(no output yet)" : st.result );
    _list.title = "workflows › #" + std::to_string(run->id) + " › step " + std::to_string(_list_sel + 1);
    _list_detail_rows = wrap_to_rows(body, term_cols() - 4);
    _list_detail = true;
    _list_detail_top = 0;
    draw_list_menu(true);
}

void InlineRepl::workflow_up() {
    if ( _list_detail ) { _list_detail = false; build_workflow_level(true); return; } // content → steps
    if ( _wf_level == 1 ) {                                                            // steps → runs
        int back = _wf_run_id;
        _wf_level = 0;
        _wf_run_id = -1;
        _list_top = 0;
        std::vector<WorkflowRun> runs = _wf_provider ? _wf_provider() : std::vector<WorkflowRun>{};
        _list_sel = 0;
        for ( size_t i = 0; i < runs.size(); ++i )
            if ( runs[i].id == back ) { _list_sel = static_cast<int>(i); break; }
        build_workflow_level(true);
        return;
    }
    close_list_menu(); // runs → close (clears _wf_active)
}

void InlineRepl::handle_workflow_key(int c) {
    int n = static_cast<int>(_list.rows.size());
    int vh = menu_view_rows();
    if ( c == 0x1b ) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv { 0, 40 * 1000 };
        if ( select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0 ) {
            int b1 = read_byte();
            if ( b1 == '[' || b1 == 'O' ) {
                int b2 = read_byte();
                if ( b2 == '5' || b2 == '6' ) read_byte(); // consume '~' of PgUp/PgDn
                if ( _list_detail ) {
                    if ( b2 == 'A' ) { _list_detail_top--; draw_list_menu(true); }
                    else if ( b2 == 'B' ) { _list_detail_top++; draw_list_menu(true); }
                    else if ( b2 == '5' ) { _list_detail_top -= vh; draw_list_menu(true); }
                    else if ( b2 == '6' ) { _list_detail_top += vh; draw_list_menu(true); }
                } else if ( n > 0 ) {
                    if ( b2 == 'A' ) { _list_sel = std::max(0, _list_sel - 1); draw_list_menu(true); }
                    else if ( b2 == 'B' ) { _list_sel = std::min(n - 1, _list_sel + 1); draw_list_menu(true); }
                    else if ( b2 == '5' ) { _list_sel = std::max(0, _list_sel - vh); draw_list_menu(true); }
                    else if ( b2 == '6' ) { _list_sel = std::min(n - 1, _list_sel + vh); draw_list_menu(true); }
                }
            }
            return;
        }
        workflow_up(); // bare Esc goes up a level (closes at the top)
        return;
    }
    if ( _list_detail )
        return; // only scroll / esc in the content view
    if ( c == '\r' || c == '\n' ) { workflow_enter(); return; }
    // Actions at the runs level: c cancel, r retry (routed through the text command).
    if ( _wf_level == 0 && ( c == 'c' || c == 'r' )) {
        if ( _list_sel < static_cast<int>(_list.keys.size()) && !_list.keys[_list_sel].empty() && _command_cb ) {
            std::string id = _list.keys[_list_sel];
            _command_cb(std::string("/workflows ") + ( c == 'c' ? "cancel " : "retry " ) + id);
            build_workflow_level(true); // refresh in place
        }
        return;
    }
}

void InlineRepl::close_list_menu() {
    if ( _list_lines > 0 )
        wr("\r\033[" + std::to_string(_list_lines) + "A\033[J");
    wr("\033[?25h"); // restore the cursor
    _list_lines = 0;
    _in_list = false;
    _list_detail = false;
    _wf_active = false;
    bool has_pending;
    {
        std::lock_guard<std::mutex> lk(_mx);
        has_pending = !_pending.empty();
    }
    if ( has_pending && !_turn_running )
        drain_pending();
    else
        draw_live();
}

void InlineRepl::apply_settings_edit() {
    SettingRow& row = _settings_rows[_settings_selection];
    std::string val = common::trim_ws(_settings_edit_buf);
    _settings_editing = false;
    if ( !val.empty()) {
        if ( _command_cb )
            _command_cb("/settings " + row.key + " " + val);
        row.value = val;
    }
    draw_settings_menu(true);
}

void InlineRepl::handle_settings_key(int c) {
    int n = static_cast<int>(_settings_rows.size());
    if ( n == 0 ) { close_settings_menu(); return; }

    // Free-text edit mode: type a value into the selected row.
    if ( _settings_editing ) {
        if ( c == 0x1b ) {
            // Consume a possible arrow sequence (ignored while editing); a bare
            // Esc cancels the edit and returns to menu navigation.
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            struct timeval tv { 0, 40 * 1000 };
            if ( select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0 ) {
                int b1 = read_byte();
                if ( b1 == '[' || b1 == 'O' )
                    read_byte();
                return;
            }
            _settings_editing = false;
            draw_settings_menu(true);
            return;
        }
        if ( c == '\r' || c == '\n' ) {
            apply_settings_edit();
            return;
        }
        if ( c == 0x7f || c == 0x08 ) { // backspace (one UTF-8 char)
            if ( !_settings_edit_buf.empty()) {
                _settings_edit_buf.pop_back();
                while ( !_settings_edit_buf.empty() &&
                        (static_cast<unsigned char>(_settings_edit_buf.back()) & 0xC0) == 0x80 )
                    _settings_edit_buf.pop_back();
            }
            draw_settings_menu(true);
            return;
        }
        if ( c >= 0x20 ) { // printable / UTF-8 byte
            _settings_edit_buf += static_cast<char>(c);
            draw_settings_menu(true);
            return;
        }
        return; // ignore other control keys while editing
    }

    // Navigation mode.
    if ( c == 0x1b ) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv { 0, 40 * 1000 };
        if ( select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0 ) {
            int b1 = read_byte();
            if ( b1 == '[' || b1 == 'O' ) {
                int b2 = read_byte();
                auto change = [&](int dir) {
                    if ( _settings_rows[_settings_selection].is_number ) adjust_number_row(dir);
                    else cycle_settings_row(dir);
                };
                if ( b2 == 'A' )      { _settings_selection = (_settings_selection - 1 + n) % n; draw_settings_menu(true); }
                else if ( b2 == 'B' ) { _settings_selection = (_settings_selection + 1) % n; draw_settings_menu(true); }
                else if ( b2 == 'C' ) change(+1); // right
                else if ( b2 == 'D' ) change(-1); // left
            }
            return;
        }
        close_settings_menu(); // bare Esc closes the menu
        return;
    }

    if ( c == '\r' || c == '\n' ) {
        SettingRow& row = _settings_rows[_settings_selection];
        // Only free-text rows react to Enter (start editing). Enum rows change
        // with ←/→ only, so Enter here does nothing — it must not flip the value
        // the user just picked.
        if ( row.options.empty()) {
            _settings_editing = true;
            _settings_edit_buf = ( row.value == "unlimited" ) ? "" : row.value;
            draw_settings_menu(true);
        }
        return;
    }
    // Other keys ignored.
}

void InlineRepl::handle_confirm_key(int c) {
    // Note sub-mode: type the deny reason. Enter submits, Esc cancels back to the
    // menu, Backspace edits; printable bytes append.
    if ( _confirm_note_mode ) {
        if ( c == '\r' || c == '\n' ) {
            std::string reason = common::trim_ws(_confirm_note_buf);
            commit_confirm(tools::Decision::deny,
                           reason.empty() ? "denied" : "denied: " + reason);
        } else if ( c == 0x1b ) {
            _confirm_note_mode = false;
            _confirm_note_buf.clear();
            draw_confirm_menu(_confirm_req, true); // clear the note line before redrawing the menu
        } else if ( c == 0x7f || c == 0x08 ) {
            if ( !_confirm_note_buf.empty()) {
                _confirm_note_buf.pop_back();
                while ( !_confirm_note_buf.empty() &&
                        (static_cast<unsigned char>(_confirm_note_buf.back()) & 0xC0) == 0x80 )
                    _confirm_note_buf.pop_back();
            }
            draw_confirm_menu(_confirm_req, true);
        } else if ( c >= 0x20 ) {
            _confirm_note_buf += static_cast<char>(c);
            draw_confirm_menu(_confirm_req, true);
        }
        return;
    }

    int nopts = confirm_choices(_confirm_req).size();

    if ( c == 0x1b ) {
        // Distinguish an arrow key (ESC [ A/B) from a bare Esc (deny).
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv { 0, 40 * 1000 };
        if ( select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0 ) {
            int b1 = read_byte();
            if ( b1 == '[' || b1 == 'O' ) {
                int b2 = read_byte();
                if ( b2 == 'A' ) {       // up
                    _confirm_selection = (_confirm_selection - 1 + nopts) % nopts;
                    draw_confirm_menu(_confirm_req, true);
                } else if ( b2 == 'B' ) { // down
                    _confirm_selection = (_confirm_selection + 1) % nopts;
                    draw_confirm_menu(_confirm_req, true);
                }
            }
            return; // other escape sequences ignored
        }
        commit_confirm(tools::Decision::deny, "denied");
        return;
    }

    if ( c == '\r' || c == '\n' ) {
        auto choices = confirm_choices(_confirm_req);
        int sel = ( _confirm_selection >= 0 && _confirm_selection < static_cast<int>(choices.size()))
                  ? _confirm_selection : 0;
        // "Deny with a reason": switch to the note input instead of committing.
        if ( choices[sel].first == "Deny with a reason" ) {
            _confirm_note_mode = true;
            _confirm_note_buf.clear();
            draw_confirm_menu(_confirm_req, true); // clear the option list before the note input
            return;
        }
        tools::Decision d = choices[sel].second;
        static const std::map<tools::Decision, std::string> labels = {
            { tools::Decision::deny,    "denied" },
            { tools::Decision::once,    "allowed once" },
            { tools::Decision::turn,    "allowed for the rest of this turn" },
            { tools::Decision::session, "allowed for this session" },
            { tools::Decision::similar, "allowed all `" + std::string() },
        };
        std::string label = d == tools::Decision::similar
            ? ( "allowed all `" + _confirm_req.similar_key + "`" )
            : labels.at(d);
        commit_confirm(d, label);
        return;
    }

    // Every other key (including letters) is deliberately ignored.
}

void InlineRepl::handle_byte(int c) {
    // Follow-up to a lone ESC whose next byte was delayed past the peek window:
    // ESC then Enter inserts a newline (Alt+Enter typed as two keys); any other
    // key means the ESC was standalone, so fall through and handle this key.
    if ( _esc_pending ) {
        _esc_pending = false;
        if ( c == '\r' || c == '\n' ) { insert_text("\n"); draw_live(); return; }
    }

    switch ( c ) {
        case '\r': // Enter (Carriage Return) submits the line
            on_enter();
            return;
        case '\n': // Ctrl-J (Line Feed) inserts a newline
            insert_text("\n");
            draw_live();
            return;
        case 0x7f: // DEL
        case 0x08: // Backspace
            backspace();
            draw_live();
            return;
        case 0x03: // Ctrl-C
            if ( _turn_running ) {
                // Interrupt the in-flight turn (works even if the request hangs).
                agent::turn_abort.store(true, std::memory_order_relaxed);
            } else if ( !_input.empty()) {
                _input.clear();
                _cursor = 0;
                _input_window_start = 0;
                _pastes.clear();
                draw_live();
            } else {
                agent::running.store(false, std::memory_order_relaxed);
            }
            return;
        case 0x04: // Ctrl-D
            if ( _input.empty())
                agent::running.store(false, std::memory_order_relaxed);
            return;
        case 0x01: // Ctrl-A -> start of line
            _cursor = 0;
            draw_live();
            return;
        case 0x05: // Ctrl-E -> end of line
            _cursor = _input.size();
            draw_live();
            return;
        case 0x17: // Ctrl-W -> delete the word before the cursor
            delete_word_before();
            draw_live();
            return;
        case 0x15: // Ctrl-U -> delete to the start of the line
            kill_to_line_start();
            draw_live();
            return;
        case 0x0b: // Ctrl-K -> delete to the end of the line
            kill_to_line_end();
            draw_live();
            return;
        case 0x09: // Tab -> autocomplete slash commands / file paths
            handle_tab();
            return;
        case 0x1b: { // ESC: control sequence
            // Peek for a follow-up byte. A bare ESC has none, and a blocking read
            // here would freeze the whole event loop (streaming + spinner) until
            // the next keypress. Escape sequences arrive as one burst, so if
            // nothing is waiting within a short window it was a lone ESC — ignore.
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            struct timeval tv { 0, 40 * 1000 };
            if ( select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0 ) {
                _esc_pending = true; // a lone ESC so far; decide on the next key
                return;
            }
            int b1 = read_byte();
            if ( b1 == '\r' || b1 == '\n' ) { // Alt+Enter inserts a newline
                insert_text("\n");
                draw_live();
                return;
            }
            if ( b1 != '[' && b1 != 'O' )
                return; // lone ESC / unsupported
            std::string seq;
            int fb;
            while ( (fb = read_byte()) >= 0 ) {
                if ( (fb >= '0' && fb <= '9') || fb == ';' ) {
                    seq += static_cast<char>(fb);
                    continue;
                }
                seq += static_cast<char>(fb);
                break; // final byte
            }
            if ( seq == "D" ) { move_left(); draw_live(); }
            else if ( seq == "C" ) { move_right(); draw_live(); }
            else if ( seq == "A" ) { if ( !_config.multiline || !multiline_vertical(-1)) history_prev(); draw_live(); }
            else if ( seq == "B" ) { if ( !_config.multiline || !multiline_vertical(+1)) history_next(); draw_live(); }
            else if ( seq == "H" || seq == "1~" || seq == "7~" ) { _cursor = 0; draw_live(); }
            else if ( seq == "F" || seq == "4~" || seq == "8~" ) { _cursor = _input.size(); draw_live(); }
            else if ( seq == "3~" ) { // Delete (forward)
                if ( _cursor < _input.size()) {
                    auto box = placeholder_starting_at(_cursor);
                    if ( box.second != std::string::npos ) {
                        std::string token = _input.substr(box.first, box.second - box.first);
                        _input.erase(box.first, box.second - box.first);
                        drop_paste(token);
                    } else {
                        size_t n = next_char(_cursor);
                        _input.erase(_cursor, n - _cursor);
                    }
                    draw_live();
                }
            }
            else if ( seq == "200~" ) { read_bracketed_paste(); draw_live(); }
            return;
        }
        default:
            break;
    }

    if ( c >= 0x20 ) { // printable ASCII or a UTF-8 lead/continuation byte (>= 0x80)
        // Printable byte: read the rest of the UTF-8 sequence if any.
        std::string ch(1, static_cast<char>(c));
        int extra = 0;
        if ( (c & 0xE0) == 0xC0 ) extra = 1;
        else if ( (c & 0xF0) == 0xE0 ) extra = 2;
        else if ( (c & 0xF8) == 0xF0 ) extra = 3;
        for ( int i = 0; i < extra; ++i ) {
            int b = read_byte();
            if ( b < 0 )
                break;
            ch += static_cast<char>(b);
        }
        insert_text(ch);
        draw_live();
    }
}

// ── tool confirmation ───────────────────────────────────────────────────

tools::Decision InlineRepl::confirm_on_main(const tools::ConfirmRequest& req, std::string& note) {
    // Main-thread confirm: render the dialog and drive the key loop here, since
    // the event loop that normally services confirm() is not running yet.
    _confirm_req = req;
    _confirm_answered = false;
    _confirm_note.clear();
    render_confirm_dialog(req);
    _confirming = true;
    while ( _confirming && agent::running.load(std::memory_order_relaxed)) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv { 0, 80 * 1000 };
        int r = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
        if ( r > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
            int c = read_byte();
            if ( c < 0 ) {
                if ( errno == EINTR ) continue;
                break;
            }
            handle_confirm_key(c);
        }
    }
    note = _confirm_note;
    return _confirm_answered ? _confirm_decision : tools::Decision::deny;
}

tools::Decision InlineRepl::confirm(const tools::ConfirmRequest& req, std::string& note) {
    // Called on the worker thread. Hand the request to the main (UI) thread and
    // block until it renders the prompt and reads the user's choice.
    std::unique_lock<std::mutex> lk(_mx);
    _confirm_req = req;
    _confirm_answered = false;
    _confirm_pending = true;
    _confirm_note.clear();
    _cv.wait(lk, [this]() {
        return _confirm_answered || !agent::running.load(std::memory_order_relaxed);
    });
    _confirm_pending = false;
    note = _confirm_note;
    return _confirm_answered ? _confirm_decision : tools::Decision::deny;
}

std::string InlineRepl::ask_user(const std::string& question, const std::vector<std::string>& options) {
    // Worker thread: hand the question to the main thread and block until answered.
    std::unique_lock<std::mutex> lk(_mx);
    _ask_question = question;
    _ask_options = options;
    _ask_answered = false;
    _ask_answer.clear();
    _ask_pending = true;
    _cv.wait(lk, [this]() {
        return _ask_answered || !agent::running.load(std::memory_order_relaxed);
    });
    _ask_pending = false;
    return _ask_answered ? _ask_answer : std::string();
}

void InlineRepl::render_ask_dialog() {
    erase_live();
    tcflush(STDIN_FILENO, TCIFLUSH);
    wr("\033[?25l");
    // An ask_user prompt is the lowest-threshold bell event: the model is blocked
    // waiting for your decision — exactly the "you looked away" case.
    if ( bell_level(_config.bell) >= 1 )
        wr("\a");
    // Word-wrap the question with a hanging indent so wrapped lines align under
    // the question TEXT (column 3, same as the options below), not back at the
    // ❓ marker in column 0.
    {
        int qwidth = term_cols() - 4;
        if ( qwidth < 8 ) qwidth = 8;
        std::vector<std::string> qsegs = word_wrap(_ask_question, qwidth);
        wr("\n");
        for ( size_t i = 0; i < qsegs.size(); ++i ) {
            std::string pfx = ( i == 0 ) ? "❓ " : "   ";
            wr(_theme.command + pfx + qsegs[i] + Theme::reset + "\n");
        }
        wr("\n");
    }
    _ask_sel = 0;
    _ask_input.clear();
    _ask_menu_lines = 0;
    draw_ask_menu(false);
}

void InlineRepl::draw_ask_menu(bool redraw) {
    std::string out;
    if ( redraw && _ask_menu_lines > 0 )
        out += "\r\033[" + std::to_string(_ask_menu_lines) + "A";
    out += "\033[J";
    int lines = 0;
    if ( _ask_options.empty()) {
        // Free-text answer.
        out += _theme.dim + "type your answer (Enter to send, Esc to skip):" + Theme::reset + "\r\n";
        out += "\033[1;7m ❯ \033[0m " + _ask_input + "\r\n";
        lines = 2;
    } else {
        // Word-wrap each option to the terminal width and count its ACTUAL
        // physical rows: a long option wraps onto several rows, and if the line
        // count is off, the redraw backs up too few rows and leaves stale copies
        // behind (the same one-row-per-item trap as the status/reader).
        int width = term_cols() - 4;
        if ( width < 8 ) width = 8;
        for ( size_t i = 0; i < _ask_options.size(); ++i ) {
            bool sel = ( static_cast<int>(i) == _ask_sel );
            std::vector<std::string> segs = word_wrap(_ask_options[i], width);
            for ( size_t s = 0; s < segs.size(); ++s ) {
                if ( sel )
                    out += "\033[1;7m " + std::string( s == 0 ? "❯ " : "  " ) + segs[s] + " \033[0m\r\n";
                else
                    out += "   " + segs[s] + "\r\n";
                lines++;
            }
        }
        out += _theme.dim + "  ↑↓ move · ⏎ pick · esc skip" + Theme::reset + "\r\n";
        lines++;
    }
    wr(out);
    _ask_menu_lines = lines;
}

void InlineRepl::handle_ask_key(int c) {
    if ( _ask_options.empty()) {
        // Free-text entry.
        if ( c == '\r' || c == '\n' ) { commit_ask(common::trim_ws(_ask_input)); return; }
        if ( c == 0x1b ) { commit_ask(""); return; } // esc = skip
        if ( c == 0x7f || c == 0x08 ) {
            if ( !_ask_input.empty()) {
                _ask_input.pop_back();
                while ( !_ask_input.empty() &&
                        (static_cast<unsigned char>(_ask_input.back()) & 0xC0) == 0x80 )
                    _ask_input.pop_back();
            }
            draw_ask_menu(true);
            return;
        }
        if ( c >= 0x20 ) { _ask_input += static_cast<char>(c); draw_ask_menu(true); }
        return;
    }
    int n = static_cast<int>(_ask_options.size());
    if ( c == 0x1b ) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv { 0, 40 * 1000 };
        if ( select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0 ) {
            int b1 = read_byte();
            if ( b1 == '[' || b1 == 'O' ) {
                int b2 = read_byte();
                if ( b2 == 'A' ) { _ask_sel = ( _ask_sel - 1 + n ) % n; draw_ask_menu(true); }
                else if ( b2 == 'B' ) { _ask_sel = ( _ask_sel + 1 ) % n; draw_ask_menu(true); }
            }
            return;
        }
        commit_ask(""); // bare esc = skip
        return;
    }
    if ( c == '\r' || c == '\n' ) {
        if ( _ask_sel >= 0 && _ask_sel < n )
            commit_ask(_ask_options[_ask_sel]);
        return;
    }
}

void InlineRepl::commit_ask(const std::string& answer) {
    if ( _ask_menu_lines > 0 )
        wr("\r\033[" + std::to_string(_ask_menu_lines) + "A\033[J");
    wr("\033[?25h");
    wr("\033[1m→ " + ( answer.empty() ? std::string("(skipped)") : answer ) + "\033[0m\n");
    _ask_menu_lines = 0;
    {
        std::lock_guard<std::mutex> lk(_mx);
        _ask_answer = answer;
        _ask_answered = true;
        _ask_pending = false;
    }
    _cv.notify_all();
    _asking = false;
    draw_live();
}

} // namespace agent
