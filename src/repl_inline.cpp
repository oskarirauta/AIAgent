#include "agent/repl_inline.hpp"
#include <set>

#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
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

// Terminal-bell policy as an ordered level: never < question < attention < always.
// An event rings if the configured level is at least the event's threshold.
static int bell_level(const std::string& mode) {
    if ( mode == "never" ) return 0;
    if ( mode == "question" ) return 1;
    if ( mode == "always" ) return 3;
    return 2; // attention (default)
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
        "/about", "/info", "/help", "/theme", "/settings", "/workflows",
        "/trust", "/history", "/memories", "/tasks", "/skills", "/pins",
        "/context", "/cost", "/changes", "/mcp",
        // settings that only affect the NEXT request — running them mid-turn just
        // updates local state (last value wins: /effort medium then /effort max
        // leaves only max, a natural dedup), applied before the next prompt. They
        // don't change the running turn's tool gating or rebuild the conversation.
        "/effort", "/thinking", "/stream", "/model", "/autoresume", "/bell"
    };
    return immediate.count(cmd) > 0;
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

// ── construction / teardown ─────────────────────────────────────────────

InlineRepl::InlineRepl(callback_t cb, Config& config, const Conversation& conversation, const TokenStats& stats)
    : _callback(std::move(cb)), _config(config), _conversation(conversation), _stats(stats) {
    _theme = theme_by_name(config.theme);
}

std::string InlineRepl::apply_theme_command(const std::string& line) {
    std::string arg;
    {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;       // "/theme"
        iss >> arg;       // name
    }
    if ( arg.empty())
        return "theme: " + _theme.name + "  (available: dark, light, warm)";
    if ( arg != "dark" && arg != "light" && arg != "warm" )
        return "unknown theme: " + arg + "  (available: dark, light, warm)";
    _theme = theme_by_name(arg);
    _config.theme = _theme.name; // keep config in sync so the choice is persisted
    return "theme: " + _theme.name;
}

InlineRepl::~InlineRepl() {
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
    int w = 0;
    for ( unsigned char c : s )
        if ( (c & 0xC0) != 0x80 ) // count only UTF-8 lead bytes
            ++w;
    return w;
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

        bool styled = !color.empty() || sp.bold;
        if ( styled ) {
            if ( sp.bold ) out += "\033[1m";
            out += color;
        }
        out += sp.text;
        if ( styled )
            out += Theme::reset;
    }
    return out;
}

void InlineRepl::emit_styled_line(const std::string& line) {
    // Each reply line is left-padded 2 columns (matching the "> " on user
    // messages); combined with the wrap width below this leaves a 2-column right
    // margin. The very first line of a reply gets the AI marker instead of pad.
    auto next_prefix = [this]() -> std::string {
        if ( _reply_first_line ) {
            _reply_first_line = false;
            if ( _reply_dim )
                return _theme.dim + "💭 " + Theme::reset;
            return _theme.ai + "● " + Theme::reset;
        }
        return "  ";
    };

    // Thinking region: dim, no syntax highlighting, word-wrapped like prose.
    if ( _reply_dim ) {
        int width = term_cols() - 4;
        if ( width < 8 ) width = 8;
        std::vector<std::string> segs = word_wrap(line, width);
        for ( size_t i = 0; i < segs.size(); ++i ) {
            wr(next_prefix() + _theme.dim + segs[i] + Theme::reset);
            if ( i + 1 < segs.size())
                wr("\n");
        }
        return;
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
        return;
    }

    if ( _in_code ) {
        // Code is left unwrapped (breaking at spaces would be wrong); the
        // terminal soft-wraps it so a copy stays faithful.
        wr(next_prefix() + style_spans(line, _code_lang));
        return;
    }

    // Prose: word-wrap so lines don't break mid-word, within the padded width.
    int width = term_cols() - 4;
    if ( width < 8 ) width = 8;
    std::vector<std::string> segs = word_wrap(line, width);
    for ( size_t i = 0; i < segs.size(); ++i ) {
        wr(next_prefix() + style_spans(segs[i], Language::markdown));
        if ( i + 1 < segs.size())
            wr("\n");
    }
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
            emit(_theme.dim + sanitize_display(plines[i]) + "\033[0m");
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

void InlineRepl::begin_reply() {
    _in_reply = true;
    _line_buf.clear();
    _in_code = false;
    _code_lang = Language::none;
    _pending_blanks = 0;
    _reply_has_content = false;
    _reply_first_line = true;
    _reply_dim = false;
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

    if ( common::trim_ws(line).empty()) {
        // Defer blank lines: interior ones are flushed once more content arrives;
        // leading ones are skipped; trailing ones are simply never flushed.
        if ( _reply_has_content )
            ++_pending_blanks;
        return;
    }

    if ( !_reply_has_content ) {
        wr("\n");                 // the single blank line before the reply
        _reply_has_content = true;
    } else {
        for ( int i = 0; i < _pending_blanks; ++i )
            wr("\n");
    }
    _pending_blanks = 0;

    emit_styled_line(line);
    wr("\n");
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
    for ( char ch : chunk ) {
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

    std::string header = _theme.dim + "💭 thinking";
    if ( first > 0 ) header += " (+" + std::to_string(first) + " earlier)";
    header += "…" + std::string(Theme::reset);
    out.push_back(header);
    for ( int i = first; i < total; ++i )
        out.push_back("  " + _theme.dim + wrapped[i] + Theme::reset);
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
        std::string next_queued;
        {
            std::lock_guard<std::mutex> lk(_mx);
            activity = _activity;
            streamed = _turn_streamed;
            queued = _pending.size();
            if ( queued > 0 )
                next_queued = _pending.front();
        }
        std::string what = !activity.empty() ? activity : ( streamed ? "responding" : "thinking");

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
            for ( char& c : next_queued )
                if ( c == '\n' || c == '\t' ) c = ' ';
            qclip = clip(next_queued, 24);
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
            if ( !q_suffix ) return 0;
            int w = 3 + static_cast<int>(std::to_string(queued).size()) + 7; // " · N queued"
            if ( q_preview ) w += 3 + display_width(qclip) + 1;              // ": “…”"
            return w;
        };
        auto room = [&]() { return cols - fixed - static_cast<int>(hint.size()) - qwidth(); };

        if ( room() < 12 ) hint.clear();
        if ( room() < 12 ) q_preview = false;
        if ( room() < 12 ) q_suffix = false;
        what = clip(what, std::max(1, room()));

        // Pre-styled: a bright spinner + label stands out against the dim idle
        // status line. (draw_live prints this verbatim while a turn is running.)
        std::string s = _theme.accent + std::string(frame) + Theme::reset + " "
                      + _theme.accent + what + " " + secs_s + Theme::reset;
        if ( !hint.empty())
            s += _theme.dim + hint + Theme::reset;
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
                        : (_config.confirm_tools ? "tools: confirm" : "tools: auto");
    if ( _config.plan_mode )
        tools += " · plan";

    std::string s = _config.provider + " · " + _config.model + " · " + cwd + " · " + tools;

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
                                           _stats.session_cached.load(std::memory_order_relaxed));
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
    notify_quiet(line);
}

bool InlineRepl::enqueue_prompt(const std::string& text) {
    std::lock_guard<std::mutex> lk(_mx);
    if ( _auto_since_user >= 2 )
        return false; // chain guard: results still fold in on the next user message
    ++_auto_since_user;
    _pending.push_back(text);
    return true;
}

void InlineRepl::drain_notices() {
    std::vector<Notice> lines;
    {
        std::lock_guard<std::mutex> lk(_mx);
        while ( !_notices.empty()) {
            lines.push_back(_notices.front());
            _notices.pop();
        }
    }
    if ( lines.empty())
        return;
    erase_live();
    bool ring = false;
    for ( const auto& n : lines ) {
        if ( n.bell ) {
            wr(_theme.accent + "● " + n.text + Theme::reset + "\r\n");
            ring = true;
        } else {
            wr(_theme.dim + n.text + Theme::reset + "\r\n");
        }
    }
    if ( ring && bell_level(_config.bell) >= 2 ) // a notice (workflow done) is "attention"
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
        "/help", "/about", "/settings", "/provider", "/model", "/btw", "/note",
        "/tools", "/strict", "/thinking", "/effort", "/theme", "/stream",
        "/memories", "/context", "/cost", "/history", "/retry", "/undo", "/tasks",
        "/pin", "/pins", "/unpin", "/queue", "/trust", "/skills", "/skill", "/plan",
        "/changes", "/export", "/compact", "/clear", "/reset", "/mcp", "/advisor",
        "/workflows", "/exit", "/quit"
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
    } else if ( !token.empty() || !at_prefix.empty()) {
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
    auto is_space = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\n'; };
    size_t p = _cursor;
    while ( p > 0 && is_space(_input[p - 1]) ) --p;
    while ( p > 0 && !is_space(_input[p - 1]) ) --p;
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
        if ( !_confirming && !_in_settings && !_in_list )
            drain_notices();

        // Idle + something queued (e.g. a workflow auto-resume prompt enqueued
        // from a background thread): run it through the normal turn machinery.
        if ( !_turn_running && !_confirming && !_in_settings && !_in_list ) {
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
                draw_settings_menu(false);
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
                    while ( budget-- > 0 && !_confirming && !_in_settings && !_in_list ) {
                        int rem = 0;
                        if ( ioctl(STDIN_FILENO, FIONREAD, &rem) != 0 || rem <= 0 )
                            break;
                        int b = read_byte();
                        if ( b < 0 )
                            break;
                        handle_byte(b);
                    }
                    _defer_draw = false;
                    draw_live();
                } else {
                    handle_byte(c);
                }
            }
        } else if ( r < 0 && errno != EINTR ) {
            break;
        } else if ( _turn_running && !_confirming && !_in_settings && !_in_list ) {
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
            {
                std::lock_guard<std::mutex> lk(_mx);
                _pending.push_back(trimmed);
            }
            tcflush(STDIN_FILENO, TCIFLUSH);
            draw_live();
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
        {
            std::lock_guard<std::mutex> lk(_mx);
            _pending.push_back(line);
            _auto_since_user = 0; // real user input resets the auto-resume guard
        }
        _input.clear();
        _cursor = 0;
        _input_window_start = 0;
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
    _pastes.clear();
    start_turn(line, display);
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
    // A modal menu (opened mid-turn) owns the screen — don't let streamed output
    // draw over it. The chunks stay buffered and flush when the menu closes.
    if ( _in_settings || _in_list )
        return;

    std::vector<std::string> chunks;
    bool done = false;
    bool need_confirm = false;
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
                _line_buf += c;
        }
        flush_lines();
        draw_live();
    }

    if ( need_confirm ) {
        render_confirm_dialog(req);
        _confirming = true;
        return;
    }

    if ( done && !_confirming ) {
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
                _line_buf += _out_chunks.front();
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
        int need = is_question ? 1 /*question*/ : 3 /*always*/;
        if ( bell_level(_config.bell) >= need )
            wr("\a");
    }

    // Warn once when the session crosses 80% / 100% of a configured cost/token budget.
    std::string warn = budget_warning();
    if ( !warn.empty())
        wr("\n" + _theme.warn + "⚠ " + warn + Theme::reset + "\n");

    // If the context is now near its budget, summarise it before anything else
    // (so queued messages run against the smaller history). The async compaction
    // drains the pending queue itself when it finishes.
    if ( maybe_auto_compact())
        return;

    // Run whatever was queued while the turn was in flight.
    drain_pending();
}

std::string InlineRepl::budget_warning() {
    long in = _stats.session_input.load(std::memory_order_relaxed);
    long out = _stats.session_output.load(std::memory_order_relaxed);
    long total = in + out;

    double frac = 0.0;
    std::string detail;
    if ( _config.budget_usd > 0.0 ) {
        double cost = _config.session_cost(in, out, _stats.session_cached.load(std::memory_order_relaxed));
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
    size_t budget = _config.context_budget();
    if ( budget == 0 )
        return false; // no known budget (unlimited / unknown window) — nothing to measure against
    long ctx = _stats.context_tokens.load(std::memory_order_relaxed);
    if ( ctx <= 0 )
        return false; // no usage reported yet
    size_t pct = ( _config.auto_compact_pct >= 10 && _config.auto_compact_pct <= 100 )
               ? _config.auto_compact_pct : 80;
    if ( static_cast<size_t>(ctx) < budget * pct / 100 )
        return false;
    // Need at least a couple of exchanges to be worth summarising; compact_history
    // itself declines a near-empty history, but avoid the spinner flash for it.
    int non_system = 0;
    for ( const auto& m : _conversation.messages())
        if ( m.role != Role::SYSTEM ) ++non_system;
    if ( non_system < 4 )
        return false;

    int used = static_cast<int>(static_cast<long long>(ctx) * 100 / static_cast<long long>(budget));
    start_async_command("/compact", "auto-compacting",
                        "auto-compact (context " + std::to_string(used) + "% of budget)");
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

    // Bare /queue opens a scrollable menu of the pending messages with a drop
    // action; each drop refreshes the list in place.
    if ( sub.empty()) {
        std::vector<std::string> rows, keys;
        {
            std::lock_guard<std::mutex> lk(_mx);
            for ( size_t i = 0; i < _pending.size(); ++i ) {
                std::string p = _pending[i];
                for ( char& ch : p )
                    if ( ch == '\n' || ch == '\t' ) ch = ' ';
                if ( p.size() > 100 ) p = p.substr(0, 100) + "…";
                rows.push_back(std::to_string(i + 1) + ". " + p);
                keys.push_back(std::to_string(i + 1));
            }
        }
        if ( rows.empty()) {
            render_command(line, "queue is empty");
            return;
        }
        ListMenu m;
        m.title = "queue · runs after the current turn";
        m.rows = std::move(rows);
        m.keys = std::move(keys);
        m.actions.push_back({ 'd', "/queue drop ", "drop" });
        m.reopen_cmd = "/queue";
        open_list_menu(std::move(m));
        return;
    }

    std::string result;
    {
        std::lock_guard<std::mutex> lk(_mx);
        if ( common::to_lower(sub) == "drop" ) {
            if ( common::to_lower(arg) == "all" ) {
                size_t n = _pending.size();
                _pending.clear();
                result = "dropped " + std::to_string(n) + " queued message(s)";
            } else {
                int n = 0;
                try { n = std::stoi(arg); } catch ( ... ) { n = 0; }
                if ( n < 1 || n > static_cast<int>(_pending.size()))
                    result = _pending.empty() ? "queue is empty"
                                              : "no queued message #" + arg + " (see /queue)";
                else {
                    std::string dropped = _pending[n - 1];
                    if ( dropped.size() > 60 ) dropped = dropped.substr(0, 60) + "…";
                    _pending.erase(_pending.begin() + ( n - 1 ));
                    result = "dropped #" + arg + ": " + dropped;
                }
            }
        } else {
            result = "usage: /queue [drop <n|all>]";
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
    while ( std::getline(ls, line))
        for ( const auto& seg : word_wrap(line, width))
            wr("  " + seg + "\n");
    // No trailing blank: the live block's own leading spacer separates the result
    // from the prompt (and from a following command), so adding one here would
    // double the gap when commands are chained.
    draw_live();
}

void InlineRepl::render_confirm_dialog(const tools::ConfirmRequest& req) {
    erase_live();

    // Attention cue: if the turn has already run unattended for a few seconds,
    // ring the bell once — the agent is now blocked waiting on you.
    auto ran = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - _turn_start).count();
    // A tool-permission prompt is an "attention" event.
    if ( ran >= 4 && !_confirm_belled && bell_level(_config.bell) >= 2 ) {
        wr("\a");
        _confirm_belled = true;
    }

    // Discard anything typed before the prompt appeared so type-ahead can never
    // select an option — the whole point is that a stray keypress cannot approve.
    tcflush(STDIN_FILENO, TCIFLUSH);
    wr("\033[?25l"); // hide the cursor while the menu is up

    if ( !req.danger.empty())
        wr("\n" + _theme.danger + "⚠ dangerous command — " + req.danger + Theme::reset + "\n");
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

// ── command execution / queue ───────────────────────────────────────────

void InlineRepl::run_command_line(const std::string& trimmed) {
    // Always called when idle (no turn running).
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
        // Fall back to a curated shortlist when the provider offers no listing.
        static const std::map<std::string, std::vector<std::string>> curated = {
            { "claude",     { "claude-opus-4-8", "claude-sonnet-4-6", "claude-haiku-4-5-20251001", "claude-fable-5" }},
            { "anthropic",  { "claude-opus-4-8", "claude-sonnet-4-6", "claude-haiku-4-5-20251001", "claude-fable-5" }},
            { "openrouter", { "openrouter/auto", "openrouter/free" }},
        };
        if ( models.size() <= 1 ) {
            auto it = curated.find(_config.provider);
            if ( it != curated.end())
                for ( const auto& mdl : it->second )
                    if ( seen.insert(mdl).second ) models.push_back(mdl);
        }
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
    if ( trimmed == "/bell" ) {
        ListMenu m;
        m.title = "terminal bell";
        m.rows = { "never", "question", "attention", "always" };
        m.keys = m.rows;
        m.select_cmd = "/bell ";
        m.current = _config.bell.empty() ? "attention" : _config.bell;
        open_list_menu(std::move(m));
        return;
    }
    if ( trimmed == "/tools" || trimmed == "/stream" || trimmed == "/strict" || trimmed == "/plan" ) {
        ListMenu m;
        if ( trimmed == "/tools" ) {
            m.title = "tool confirmation";
            m.rows = { "confirm", "auto", "insecure" };
            m.current = _config.confirm_tools ? "confirm" : "auto";
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

    // Reader commands open a scrollable, dismissable list menu instead of dumping
    // a long block into the transcript (a big /workflows or /history used to flood
    // it). Works for the bare list and the detail form (e.g. /workflows 3).
    {
        std::string base = trimmed;
        size_t sp = base.find_first_of(" \t");
        if ( sp != std::string::npos ) base = base.substr(0, sp);
        static const std::set<std::string> readers = {
            "/workflows", "/history", "/memories", "/tasks", "/skills"
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
            if ( base == "/workflows" && trimmed == "/workflows" ) {
                // Only actual run rows ("#<id> …") are selectable and drillable;
                // the header/footer/blank chrome is dropped so the cursor can't
                // land on it. Enter drills into the selected run's steps.
                m.drill_cmd = "/workflows ";
                m.actions.push_back({ 'c', "/workflows cancel ", "cancel" });
                m.actions.push_back({ 'r', "/workflows retry ", "retry" });
                m.reopen_cmd = "/workflows";
                for ( const auto& row : all ) {
                    size_t h = row.find('#');
                    if ( h == std::string::npos ) continue;
                    std::string key;
                    size_t i = h + 1;
                    while ( i < row.size() && std::isdigit(static_cast<unsigned char>(row[i])))
                        key += row[i++];
                    if ( key.empty()) continue;
                    m.rows.push_back(common::trim_ws(row));
                    m.keys.push_back(key);
                }
            } else {
                for ( const auto& row : all )
                    if ( !common::trim_ws(row).empty())
                        m.rows.push_back(row);
            }
            // Nothing selectable (e.g. no workflows yet) — just show the text.
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
        std::string next;
        bool is_command = false;
        std::vector<std::string> parts; // consecutive user messages merged into one turn
        {
            std::lock_guard<std::mutex> lk(_mx);
            if ( _pending.empty())
                break;
            next = _pending.front();
            _pending.pop_front();
            is_command = !next.empty() && ( next[0] == '/' || next[0] == '!' );
            if ( !is_command ) {
                parts.push_back(next);
                while ( !_pending.empty() && !_pending.front().empty() &&
                        _pending.front()[0] != '/' && _pending.front()[0] != '!' ) {
                    parts.push_back(_pending.front());
                    _pending.pop_front();
                }
            }
        }
        if ( is_command ) {
            run_command_line(next);
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
        if ( n >= 1000 ) {
            std::string s = std::to_string(n / 1000) + "." + std::to_string((n % 1000) / 100) + "k";
            return s;
        }
        return std::to_string(n);
    };

    // Rough estimate: 4 chars ≈ 1 token (matches the context-budget trimming).
    size_t sys = 0, msg = 0;
    for ( const auto& m : _conversation.messages()) {
        size_t t = m.content.size() / 4;
        for ( const auto& tc : m.tool_calls )
            t += ( tc.arguments.size() + tc.name.size()) / 4;
        if ( m.role == Role::SYSTEM ) sys += t;
        else                          msg += t;
    }
    size_t mem = load_memories(_config.home_dir, _config.provider).size() / 4;
    size_t total = sys + msg;
    long actual = _stats.context_tokens.load(std::memory_order_relaxed);

    auto pct = [total](size_t n) -> std::string {
        if ( total == 0 ) return "0%";
        return std::to_string(static_cast<int>((100.0 * n / total) + 0.5)) + "%";
    };

    wr("\n" + _theme.command + "⚙ context" + Theme::reset + "\n\n");

    // Composition bar: system segment then conversation segment.
    const int barw = 46;
    int sysc = ( total > 0 ) ? static_cast<int>(( double(sys) / total ) * barw + 0.5) : 0;
    if ( sysc > barw ) sysc = barw;
    int msgc = barw - sysc;
    std::string blocks_sys, blocks_msg;
    for ( int i = 0; i < sysc; ++i ) blocks_sys += "█";
    for ( int i = 0; i < msgc; ++i ) blocks_msg += "█";
    wr("  " + _theme.ai + blocks_sys + _theme.command + blocks_msg + Theme::reset +
       "  " + _theme.dim + "~" + fmt(total) + " tokens" + Theme::reset + "\n\n");

    wr("  " + _theme.ai + "●" + Theme::reset + " system prompt   " + fmt(sys) + "  " +
       _theme.dim + "(" + pct(sys) + ")" + Theme::reset + "\n");
    wr("  " + _theme.command + "●" + Theme::reset + " conversation    " + fmt(msg) + "  " +
       _theme.dim + "(" + pct(msg) + ")" + Theme::reset + "\n");
    if ( mem > 0 )
        wr("    " + _theme.dim + "└ memories      " + fmt(mem) + Theme::reset + "\n");

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
    footer += Theme::reset + std::string("\n");
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
                   long lo = 0, long hi = 0, long step = 1, std::string unit = "", std::string zero = "") {
        SettingRow r;
        r.key = std::move(key); r.label = std::move(label); r.value = std::move(value);
        r.group = std::move(group); r.desc = std::move(desc); r.options = std::move(opts);
        r.is_number = number; r.num_min = lo; r.num_max = hi; r.num_step = step;
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
        "pause to ask after this many tool calls in one turn", {}, true,
        0, 500, 10, "per turn", "unlimited");
    if ( _config.provider == "claude" )
        add("advisor", "advisor", _config.advisor ? "on" : "off", TOOLS,
            "let the model consult a stronger advisor model", { "off", "on" });

    add("context", "context",
        first_word(cur.count("context") ? cur["context"] : "unlimited"), CTX,
        "token budget before older turns are trimmed (auto / a number / 0)");
    add("auto_compact", "auto-compact", _config.auto_compact ? "on" : "off", CTX,
        "summarise older history automatically as it nears the budget", { "off", "on" });
    add("max_tokens", "max reply", std::to_string(_config.max_tokens), CTX,
        "cap on a single reply's output tokens", {}, true, 256, 200000, 1024, "tokens");
    add("autoresume", "workflow resume", _config.workflow_autoresume ? "on" : "off", CTX,
        "when a background workflow finishes, feed its results to the model automatically",
        { "off", "on" });

    add("theme", "theme", _theme.name, UI,
        "colour theme (never sets the terminal background)", { "dark", "light", "warm" });
    add("bell", "bell", _config.bell, UI,
        "terminal bell: always answer · attention (workflow/tool/question) · question only · never",
        { "never", "question", "attention", "always" });
    add("multiline", "multiline", _config.multiline ? "on" : "off", UI,
        "Enter inserts a newline; Alt+Enter sends the message", { "off", "on" });
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
    std::string s = std::to_string(v);
    if ( !row.unit.empty()) s += " " + row.unit;
    return s;
}

void InlineRepl::draw_settings_menu(bool redraw) {
    int n = static_cast<int>(_settings_rows.size());
    std::string out;
    if ( redraw && _settings_menu_lines > 0 )
        out += "\r\033[" + std::to_string(_settings_menu_lines) + "A";
    out += "\033[J";

    int lines = 0;
    std::string group;
    constexpr size_t LW = 16; // label column width (fits the longest label + a gap)
    for ( int i = 0; i < n; ++i ) {
        const SettingRow& row = _settings_rows[i];
        if ( row.group != group ) {           // section header
            group = row.group;
            out += ( lines ? "\r\n" : "" ) + _theme.accent + "  " + group + Theme::reset + "\r\n";
            lines += lines ? 2 : 1;
        }
        std::string label = row.label;
        while ( label.size() < LW ) label += ' ';
        bool selected = ( i == _settings_selection );
        std::string val = setting_display_value(row);

        if ( selected && _settings_editing ) {
            out += "\033[1;7m ❯ " + label + _settings_edit_buf + "▏ \033[0m"
                 + _theme.dim + "  (⏎ apply · esc cancel)" + Theme::reset;
        } else if ( selected ) {
            // Changeable with ←/→ (enum or number) gets angle brackets; free text
            // is just shown (Enter edits it).
            std::string shown = ( row.options.empty() && !row.is_number ) ? val : "‹ " + val + " ›";
            out += "\033[1;7m ❯ " + label + shown + " \033[0m";
        } else {
            out += "   " + label + _theme.dim + val + Theme::reset; // label default, value dim
        }
        out += "\r\n";
        lines++;

        // One-line help under the selected row.
        if ( selected && !_settings_editing && !row.desc.empty()) {
            std::string help = row.desc;
            if ( row.is_number && !row.zero_label.empty())
                help += "  ·  0 = " + row.zero_label;
            out += _theme.dim + "     └ " + help + Theme::reset + "\r\n";
            lines++;
        }
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

void InlineRepl::draw_list_menu(bool redraw) {
    std::string out;
    if ( redraw && _list_lines > 0 )
        out += "\r\033[" + std::to_string(_list_lines) + "A";
    out += "\033[J";
    int lines = 0;
    int cols = term_cols() - 3;
    if ( cols < 8 ) cols = 8;
    int vh = term_rows() - 3; // reserve title + a little breathing room
    if ( vh < 3 ) vh = 3;

    if ( _list_detail ) {
        out += _theme.command + "  " + _list.title + Theme::reset + "   " +
               _theme.dim + "↑↓ scroll · esc back" + Theme::reset + "\r\n";
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
    } else {
        std::string hint = "↑↓ move · esc close";
        if ( !_list.select_cmd.empty()) hint += " · ⏎ select";
        if ( !_list.drill_cmd.empty()) hint += " · ⏎ open";
        for ( const auto& a : _list.actions )
            hint += std::string(" · ") + a.key + " " + a.label;
        out += _theme.command + "  " + _list.title + Theme::reset + "   " +
               _theme.dim + hint + Theme::reset + "\r\n";
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
    }
    wr(out);
    _list_lines = lines;
}

void InlineRepl::handle_list_key(int c) {
    int n = static_cast<int>(_list.rows.size());
    int vh = term_rows() - 3;
    if ( vh < 3 ) vh = 3;

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

    if (( c == '\r' || c == '\n' ) && !_list.drill_cmd.empty() &&
        _list_sel < static_cast<int>(_list.keys.size()) && !_list.keys[_list_sel].empty()) {
        std::string detail = _command_cb ? _command_cb(_list.drill_cmd + _list.keys[_list_sel]) : "";
        _list_detail_rows = wrap_to_rows(detail, term_cols() - 4);
        _list_detail = true;
        _list_detail_top = 0;
        draw_list_menu(true);
        return;
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

void InlineRepl::close_list_menu() {
    if ( _list_lines > 0 )
        wr("\r\033[" + std::to_string(_list_lines) + "A\033[J");
    wr("\033[?25h"); // restore the cursor
    _list_lines = 0;
    _in_list = false;
    _list_detail = false;
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

    if ( c >= 0x20 || c >= 0x80 ) {
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

} // namespace agent
