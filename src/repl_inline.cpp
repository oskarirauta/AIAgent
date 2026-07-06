#include "agent/repl_inline.hpp"

#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <sstream>
#include <algorithm>
#include <map>
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
        {
            std::lock_guard<std::mutex> lk(_mx);
            activity = _activity;
            streamed = _turn_streamed;
            queued = _pending.size();
        }
        std::string what = !activity.empty() ? activity : ( streamed ? "responding" : "thinking");

        auto secs = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - _turn_start).count();

        // Pre-styled: a bright spinner + label stands out against the dim idle
        // status line. (draw_live prints this verbatim while a turn is running.)
        std::string s = _theme.accent + std::string(frame) + Theme::reset + " "
                      + _theme.accent + what + " " + std::to_string(secs) + "s" + Theme::reset
                      + " " + _theme.dim + "(Ctrl-C to interrupt)" + Theme::reset;
        if ( queued > 0 )
            s += " " + _theme.dim + "·" + Theme::reset + " " + _theme.warn + std::to_string(queued) + " queued" + Theme::reset;
        return s;
    }

    std::string cwd;
    try { cwd = std::filesystem::current_path().string(); } catch ( ... ) { cwd = "?"; }

    std::string tools = !_config.tools_enabled ? "tools off"
                        : (_config.confirm_tools ? "tools: confirm" : "tools: auto");

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
    }
    return s;
}

void InlineRepl::set_activity(const std::string& activity) {
    std::lock_guard<std::mutex> lk(_mx);
    _activity = activity;
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

std::vector<std::pair<size_t, size_t>> InlineRepl::wrap_input(int width) const {
    if ( width < 1 ) width = 1;
    std::vector<std::pair<size_t, size_t>> lines;
    size_t line_start = 0;
    int col = 0;
    size_t i = 0;
    while ( i < _input.size()) {
        if ( _input[i] == '\n' ) {
            lines.push_back({ line_start, i });
            ++i;
            line_start = i;
            col = 0;
            continue;
        }
        size_t j = i + 1;
        while ( j < _input.size() && (static_cast<unsigned char>(_input[j]) & 0xC0) == 0x80 )
            ++j;
        if ( col == width ) {                 // wrap before this codepoint
            lines.push_back({ line_start, i });
            line_start = i;
            col = 0;
        }
        ++col;
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
    for ( const auto& vl : vlines )
        out += vl.first + vl.second + "\r\n";
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

std::string InlineRepl::expand_input() const {
    std::string out = _input;
    for ( const auto& p : _pastes ) {
        size_t pos;
        while ( (pos = out.find(p.placeholder)) != std::string::npos )
            out.replace(pos, p.placeholder.size(), p.content);
    }
    return out;
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

    // Event loop: the LLM turn runs on a worker thread, so here we just service
    // the keyboard and the worker's output/confirm queues. select() gives a
    // short timeout that both keeps the spinner animating and lets streamed
    // output appear promptly.
    while ( agent::running.load(std::memory_order_relaxed)) {
        poll_worker();

        // Terminal was resized: redraw the active view at the new width.
        if ( agent::winch_pending.exchange(false, std::memory_order_relaxed)) {
            if ( _in_settings )
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
            else if ( _in_settings )
                handle_settings_key(c);
            else
                handle_byte(c);
        } else if ( r < 0 && errno != EINTR ) {
            break;
        } else if ( _turn_running && !_confirming && !_in_settings ) {
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
    std::string line = expand_input();
    std::string trimmed = common::trim_ws(line);

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

    // Slash commands run locally (never sent to the model), even mid-turn.
    if ( trimmed[0] == '/' ) {
        _prompt_history.push_back(trimmed);
        _history_index = _prompt_history.size();
        _stashed_input.clear();
        _input.clear();
        _cursor = 0;
        _input_window_start = 0;
        _pastes.clear();

        // While a turn is streaming, queue the command so it runs when the queue
        // advances, instead of interleaving with (and cluttering) the output.
        // Flush type-ahead so keys aimed at a not-yet-open interactive menu can't
        // trigger anything by accident.
        if ( _turn_running ) {
            _pending.push(trimmed);
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
        _pending.push(line);
        _input.clear();
        _cursor = 0;
        _input_window_start = 0;
        _pastes.clear();
        draw_live();
        return;
    }

    start_turn(line, display);
}

void InlineRepl::start_turn(const std::string& line, const std::string& display) {
    echo_user(display);
    _input.clear();
    _cursor = 0;
    _input_window_start = 0;
    _pastes.clear();
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
    if ( !_pending.empty())
        drain_pending();
    else
        draw_live();
}

void InlineRepl::poll_worker() {
    if ( !_turn_running )
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

    if ( need_confirm ) {
        render_confirm_dialog(req);
        _confirming = true;
        return;
    }

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

    // If the context is now near its budget, summarise it before anything else
    // (so queued messages run against the smaller history). The async compaction
    // drains the pending queue itself when it finishes.
    if ( maybe_auto_compact())
        return;

    // Run whatever was queued while the turn was in flight.
    drain_pending();
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

static std::vector<std::string> confirm_options(const tools::ConfirmRequest& req) {
    // Safe option first so the default selection can never approve anything.
    std::vector<std::string> opts = { "Deny", "Allow once", "Allow for this session" };
    if ( req.can_similar )
        opts.push_back("Allow all `" + req.similar_key + "`");
    return opts;
}

void InlineRepl::draw_confirm_menu(const tools::ConfirmRequest& req, bool redraw) {
    std::vector<std::string> opts = confirm_options(req);
    int n = static_cast<int>(opts.size());

    std::string out;
    if ( redraw && _confirm_menu_lines > 0 )
        out += "\r\033[" + std::to_string(_confirm_menu_lines) + "A"; // back up to the first option
    out += "\033[J"; // clear the menu region
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

    // Discard anything typed before the prompt appeared so type-ahead can never
    // select an option — the whole point is that a stray keypress cannot approve.
    tcflush(STDIN_FILENO, TCIFLUSH);
    wr("\033[?25l"); // hide the cursor while the menu is up

    if ( !req.danger.empty())
        wr("\n" + _theme.danger + "⚠ dangerous command — " + req.danger + Theme::reset + "\n");
    wr("\n" + _theme.warn + "? " + req.tool + " wants to run:" + Theme::reset + "\n");
    wr(_theme.warn + req.summary + Theme::reset + "\n\n");
    wr(_theme.dim + "Select with ↑/↓ and press Enter (Esc denies). Letters do nothing." + Theme::reset + "\n");

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
        _confirm_answered = true;
        _confirm_pending = false;
    }
    _cv.notify_all();
    _confirming = false;
    draw_live();
}

// ── command execution / queue ───────────────────────────────────────────

void InlineRepl::run_command_line(const std::string& trimmed) {
    // Always called when idle (no turn running).
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
    if ( trimmed == "/compact" ) {
        // Slow LLM call — run it off-thread with a spinner instead of blocking.
        start_async_command(trimmed, "compacting");
        return;
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
    // message starts a turn (and draining stops until it finishes). Stops early
    // if a command starts a turn or opens the interactive menu.
    while ( !_pending.empty()) {
        std::string next = _pending.front();
        _pending.pop();
        if ( !next.empty() && next[0] == '/' ) {
            run_command_line(next);
            if ( _turn_running || _in_settings )
                return;
        } else {
            start_turn(next, next);
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
    _settings_rows.push_back({ "model", "model", cur.count("model") ? cur["model"] : _config.model, {} });
    _settings_rows.push_back({ "theme", "theme", _theme.name, { "dark", "light", "warm" } });
    _settings_rows.push_back({ "tools", "tools", first_word(cur["tools"]), { "confirm", "auto", "insecure" } });
    _settings_rows.push_back({ "strict", "strict",
        cur["tools"].find("(strict)") != std::string::npos ? "on" : "off", { "off", "on" } });
    _settings_rows.push_back({ "thinking", "thinking", th,
        { "off", "on", "low", "medium", "high", "xhigh", "max" } });
    _settings_rows.push_back({ "thinking_stream", "stream",
        ( !_config.thinking_stream ? "off" : ( _config.thinking_collapse ? "collapse" : "on" )),
        { "off", "on", "collapse" } });
    _settings_rows.push_back({ "context", "context",
        first_word(cur.count("context") ? cur["context"] : "unlimited"), {} });
    _settings_rows.push_back({ "auto_compact", "compact", _config.auto_compact ? "on" : "off", { "off", "on" } });
    if ( _config.provider == "claude" )
        _settings_rows.push_back({ "advisor", "advisor", _config.advisor ? "on" : "off", { "off", "on" } });
    _settings_rows.push_back({ "multiline", "multiline", _config.multiline ? "on" : "off", { "off", "on" } });
    _settings_rows.push_back({ "paste_preview", "preview",
        _config.paste_preview == 0 ? "all" : std::to_string(_config.paste_preview), {} });

    erase_live();
    tcflush(STDIN_FILENO, TCIFLUSH); // ignore anything typed before the menu opened
    wr("\033[?25l");                 // hide the cursor while the menu is up
    wr("\n" + _theme.command + "⚙ settings" + Theme::reset + "\n\n");
    wr(_theme.dim + "↑/↓ select · ←/→ change · Enter edit text · Esc close" + Theme::reset + "\n");
    _in_settings = true;
    _settings_editing = false;
    _settings_selection = 0;
    _settings_menu_lines = 0;
    draw_settings_menu(false);
}

void InlineRepl::draw_settings_menu(bool redraw) {
    int n = static_cast<int>(_settings_rows.size());
    std::string out;
    if ( redraw && _settings_menu_lines > 0 )
        out += "\r\033[" + std::to_string(_settings_menu_lines) + "A";
    out += "\033[J";
    for ( int i = 0; i < n; ++i ) {
        const SettingRow& row = _settings_rows[i];
        std::string label = row.label;
        while ( label.size() < 10 ) label += ' '; // keep a gap even for the longest label ("multiline")
        bool selected = ( i == _settings_selection );
        if ( selected && _settings_editing ) {
            // Free-text edit in place: show the buffer with a cursor bar.
            out += "\033[1;7m ❯ " + label + _settings_edit_buf + "▏ \033[0m"
                 + _theme.dim + "  (Enter apply · Esc cancel)" + Theme::reset;
        } else if ( selected ) {
            std::string hint = row.options.empty() ? "  (Enter to edit)" : "  (←/→)";
            out += "\033[1;7m ❯ " + label + row.value + hint + " \033[0m";
        } else {
            out += _theme.dim + "   " + label + row.value + Theme::reset;
        }
        out += "\r\n";
    }
    wr(out);
    _settings_menu_lines = n;
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
    // Anything queued behind the menu (while a turn ran) resumes now.
    if ( !_pending.empty())
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
                if ( b2 == 'A' )      { _settings_selection = (_settings_selection - 1 + n) % n; draw_settings_menu(true); }
                else if ( b2 == 'B' ) { _settings_selection = (_settings_selection + 1) % n; draw_settings_menu(true); }
                else if ( b2 == 'C' ) cycle_settings_row(+1); // right
                else if ( b2 == 'D' ) cycle_settings_row(-1); // left
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
    int nopts = 3 + (_confirm_req.can_similar ? 1 : 0);

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
        tools::Decision d;
        std::string label;
        switch ( _confirm_selection ) {
            case 1: d = tools::Decision::once;    label = "allowed once"; break;
            case 2: d = tools::Decision::session; label = "allowed for this session"; break;
            case 3: d = tools::Decision::similar; label = "allowed all `" + _confirm_req.similar_key + "`"; break;
            default: d = tools::Decision::deny;   label = "denied"; break;
        }
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

tools::Decision InlineRepl::confirm(const tools::ConfirmRequest& req) {
    // Called on the worker thread. Hand the request to the main (UI) thread and
    // block until it renders the prompt and reads the user's choice.
    std::unique_lock<std::mutex> lk(_mx);
    _confirm_req = req;
    _confirm_answered = false;
    _confirm_pending = true;
    _cv.wait(lk, [this]() {
        return _confirm_answered || !agent::running.load(std::memory_order_relaxed);
    });
    _confirm_pending = false;
    return _confirm_answered ? _confirm_decision : tools::Decision::deny;
}

} // namespace agent
