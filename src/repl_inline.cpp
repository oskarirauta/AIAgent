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
#include <utility>
#include <vector>

#include "agent/config.hpp"
#include "agent/conversation.hpp"
#include "agent/signal_handler.hpp"
#include "agent/text_utils.hpp"
#include "common.hpp"
#include "logger.hpp"

namespace agent {

// ── terminal state, shared with the signal handler's emergency restore ──
static struct termios g_orig_termios;
static bool g_termios_saved = false;
static InlineRepl* g_instance = nullptr;

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

InlineRepl::InlineRepl(callback_t cb, const Config& config, const Conversation& conversation, const TokenStats& stats)
    : _callback(std::move(cb)), _config(config), _conversation(conversation), _stats(stats) {
    g_instance = this;
}

InlineRepl::~InlineRepl() {
    teardown();
    g_instance = nullptr;
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
    wr("\033[0m");
    if ( g_termios_saved )
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    _raw_active = false;
}

void InlineRepl::emergency_teardown() {
    // Async-signal context: touch only the saved terminal state, no C++ objects.
    const char* reset = "\033[?2004l\033[0m";
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
        std::string sgr;
        if ( sp.color_pair == _highlighter.color_for_keyword()) sgr = "35";      // magenta
        else if ( sp.color_pair == _highlighter.color_for_string()) sgr = "32";  // green
        else if ( sp.color_pair == _highlighter.color_for_comment()) sgr = "90"; // gray
        else if ( sp.color_pair == _highlighter.color_for_number()) sgr = "33";  // yellow
        else if ( sp.color_pair == _highlighter.color_for_type()) sgr = "36";    // cyan
        else if ( sp.color_pair == _highlighter.color_for_fence()) sgr = "90";   // gray

        bool styled = !sgr.empty() || sp.bold;
        if ( styled ) {
            out += "\033[";
            if ( sp.bold ) { out += "1"; if ( !sgr.empty()) out += ";"; }
            out += sgr;
            out += "m";
        }
        out += sp.text;
        if ( styled )
            out += "\033[0m";
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
            return "\033[1;32m●\033[0m ";
        }
        return "  ";
    };

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
        wr(next_prefix() + "\033[90m" + line + "\033[0m");
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
            wr("\033[1;36m›\033[0m " + seg + "\n");
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
        std::string header = "\033[90m── pasted · " + std::to_string(plines.size()) + " lines ";
        int hw = static_cast<int>(split_cells("── pasted · " + std::to_string(plines.size()) + " lines ").size());
        for ( int i = hw; i < width; ++i ) header += "─";
        emit(header + "\033[0m");
        for ( const auto& pl : plines )
            emit("\033[90m" + sanitize_display(pl) + "\033[0m");
        std::string footer;
        for ( int i = 0; i < width; ++i ) footer += "─";
        emit("\033[90m" + footer + "\033[0m");

        pos = best + which->placeholder.size();
    }

    if ( first ) // empty message (shouldn't happen, but stay safe)
        wr("\033[1;36m›\033[0m\n");
}

void InlineRepl::begin_reply() {
    _in_reply = true;
    _line_buf.clear();
    _in_code = false;
    _code_lang = Language::none;
    _pending_blanks = 0;
    _reply_has_content = false;
    _reply_first_line = true;
}

void InlineRepl::emit_reply_line(const std::string& line) {
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
        std::string s = "\033[1;96m" + std::string(frame) + "\033[0m "
                      + "\033[96m" + what + " " + std::to_string(secs) + "s\033[0m"
                      + " \033[90m(Ctrl-C to interrupt)\033[0m";
        if ( queued > 0 )
            s += " \033[90m·\033[0m \033[93m" + std::to_string(queued) + " queued\033[0m";
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
    // The cursor rests on the prompt line, which sits one line below the
    // separator at the top of the block — step up to the separator before
    // clearing so the whole block (separator + prompt + status) is removed.
    if ( _live_lines > 0 )
        wr("\r\033[1A\033[J");
    else
        wr("\r\033[J");
    _live_lines = 0;
}

void InlineRepl::draw_live() {
    int cols = term_cols();

    // Prompt line: a fixed "> " prefix followed by a horizontal window over the
    // input. The prefix never scrolls away, "…" marks clipped ends, and one
    // right-hand column is always left blank so we never write the terminal's
    // last cell (which would trigger auto-wrap and corrupt the in-place redraw).
    const std::string prefix = "> ";
    const int prefix_w = 2;

    std::vector<std::string> in_cells = split_cells(_input);
    int total = static_cast<int>(in_cells.size());
    int cursor_pos = display_width(_input.substr(0, _cursor));

    int win = cols - prefix_w - 1;
    if ( win < 1 )
        win = 1;

    int start = static_cast<int>(_input_window_start);
    if ( total <= win ) {
        start = 0;
    } else {
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

    std::string prompt = prefix;
    for ( const auto& c : vis ) {
        if ( c == "\n" )
            prompt += "\033[90m↵\033[0m"; // newline shown as a dim, single-cell glyph
        else
            prompt += c;
    }
    int cursor_col = prefix_w + (cursor_pos - start);

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

    // Separator rule dividing the transcript above from the input below.
    std::string sep;
    for ( int i = 0; i < cols; ++i )
        sep += "─"; // ─

    // Reposition to the top of the block: on a redraw the cursor is on the prompt
    // line (one below the separator); on a fresh draw the block does not exist yet.
    // Input line sits between two separators (transcript above, status below) so
    // the status text is never mistaken for the user's own typing.
    std::string out = ( _live_lines > 0 ) ? "\r\033[1A\033[J" : "\r\033[J";
    out += "\033[90m" + sep + "\033[0m\r\n"; // separator: transcript | input
    out += prompt;
    out += "\r\n";
    out += "\033[90m" + sep + "\033[0m\r\n"; // separator: input | status
    out += status_prestyled ? status : ("\033[90m" + status + "\033[0m"); // status
    out += "\033[2A\r";                      // back up to the prompt line
    if ( cursor_col > 0 )
        out += "\033[" + std::to_string(cursor_col) + "C";
    wr(out);
    _live_lines = 4;
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
    wr("\033[90mType your message. /exit or /quit to leave, Ctrl-C to interrupt.\033[0m\n\n");

    _history_index = _prompt_history.size();
    draw_live();

    // Event loop: the LLM turn runs on a worker thread, so here we just service
    // the keyboard and the worker's output/confirm queues. select() gives a
    // short timeout that both keeps the spinner animating and lets streamed
    // output appear promptly.
    while ( agent::running.load(std::memory_order_relaxed)) {
        poll_worker();

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
            else
                handle_byte(c);
        } else if ( r < 0 && errno != EINTR ) {
            break;
        } else if ( _turn_running && !_confirming ) {
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

        std::string result = _command_cb ? _command_cb(trimmed)
                                         : ("unknown command: " + trimmed);
        render_command(trimmed, result);
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
        for ( const auto& c : chunks )
            _line_buf += c;
        flush_lines();
        draw_live();
    }

    if ( done && !_confirming )
        finish_turn();
}

void InlineRepl::finish_turn() {
    if ( _worker.joinable())
        _worker.join();

    std::string reply;
    bool streamed;
    {
        std::lock_guard<std::mutex> lk(_mx);
        while ( !_out_chunks.empty()) {
            _line_buf += _out_chunks.front();
            _out_chunks.pop();
        }
        reply = _turn_reply;
        streamed = _turn_streamed;
        _activity.clear();
    }

    erase_live();
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

    // Auto-send the next queued prompt, if any.
    if ( !_pending.empty()) {
        std::string next = _pending.front();
        _pending.pop();
        start_turn(next, next);
    } else {
        draw_live();
    }
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
            out += "\033[90m   " + opts[i] + "\033[0m";
        out += "\r\n";
    }
    wr(out);
    _confirm_menu_lines = n;
}

void InlineRepl::render_command(const std::string& cmd, const std::string& result) {
    erase_live();
    // A system message: the command echoed with a ⚙ marker, then its result.
    wr("\n\033[1;35m⚙\033[0m " + cmd + "\n");
    int width = term_cols() - 4;
    if ( width < 8 ) width = 8;
    std::istringstream ls(result);
    std::string line;
    while ( std::getline(ls, line))
        for ( const auto& seg : word_wrap(line, width))
            wr("  " + seg + "\n");
    wr("\n");
    draw_live();
}

void InlineRepl::render_confirm_dialog(const tools::ConfirmRequest& req) {
    erase_live();

    // Discard anything typed before the prompt appeared so type-ahead can never
    // select an option — the whole point is that a stray keypress cannot approve.
    tcflush(STDIN_FILENO, TCIFLUSH);

    if ( !req.danger.empty())
        wr("\n\033[1;31m⚠ dangerous command — " + req.danger + "\033[0m\n");
    wr("\n\033[1;33m? " + req.tool + " wants to run:\033[0m\n");
    wr("\033[33m" + req.summary + "\033[0m\n\n");
    wr("\033[90mSelect with ↑/↓ and press Enter (Esc denies). Letters do nothing.\033[0m\n");

    _confirm_selection = 0;   // Deny
    _confirm_menu_lines = 0;
    draw_confirm_menu(req, false);
}

void InlineRepl::commit_confirm(tools::Decision d, const std::string& label) {
    // Erase the menu and record the choice in the transcript.
    if ( _confirm_menu_lines > 0 )
        wr("\r\033[" + std::to_string(_confirm_menu_lines) + "A\033[J");
    wr("\033[1m→ " + label + "\033[0m\n\n");
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
    switch ( c ) {
        case '\r': // Enter — accept CR or LF so it works on every terminal
        case '\n':
            on_enter();
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
            else if ( seq == "A" ) { history_prev(); draw_live(); }
            else if ( seq == "B" ) { history_next(); draw_live(); }
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
