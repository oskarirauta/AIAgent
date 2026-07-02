#include "agent/repl_ncurses.hpp"

#include <ncurses.h>
#include <cctype>
#include <clocale>
#include <cstdlib>
#include <cwchar>
#include <algorithm>
#include <filesystem>
#include "common.hpp"
#include "logger.hpp"
#include "agent/config.hpp"
#include "agent/conversation.hpp"
#include "agent/signal_handler.hpp"

namespace agent {

// Global pointer used by atexit() to ensure endwin() is called even if the
// process exits through std::exit() before reaching NcursesRepl::teardown().
static NcursesRepl* g_active_repl = nullptr;

static void ncurses_atexit_cleanup() {
    if ( g_active_repl )
        g_active_repl->teardown();
}

NcursesRepl::NcursesRepl(callback_t cb, const Config& config, const Conversation& conversation)
    : _callback(std::move(cb)), _config(config), _conversation(conversation) {
    _suggestions = {
        "# Kerro lisää",
        "# Selitä yksinkertaisemmin",
        "# Mitä seuraavaksi?",
        "# Kiitos, tämä riittää"
    };
}

NcursesRepl::~NcursesRepl() {
    {
        std::lock_guard<std::mutex> lock(_queue_mutex);
        _running = false;
    }
    if ( _worker.joinable())
        _worker.join();
    teardown();
}

void NcursesRepl::setup() {
    // Enable the user's locale so UTF-8 and wide-character rendering work.
    std::setlocale(LC_ALL, "");

    // Redirect all logger output to a file so it does not corrupt the ncurses screen.
    std::string log_dir = _config.home_dir;
    std::string log_path = log_dir + "/agent.log";
    if ( !std::filesystem::exists(log_dir))
        std::filesystem::create_directories(log_dir);
    _log_file = std::make_unique<std::ofstream>(log_path, std::ios::app);
    if ( !_log_file || !_log_file->is_open()) {
        log_path = "/tmp/agent.log";
        _log_file = std::make_unique<std::ofstream>(log_path, std::ios::app);
    }
    if ( _log_file && _log_file->is_open()) {
        logger::stream[logger::std_out] = _log_file.get();
        logger::stream[logger::std_err] = _log_file.get();
    }

    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    timeout(50); // non-blocking input so the UI can update while the worker runs
    curs_set(0); // hide hardware cursor; we draw a fake one
    if ( has_colors()) {
        start_color();
        init_pair(1, COLOR_GREEN, COLOR_BLACK);   // title
        init_pair(2, COLOR_YELLOW, COLOR_BLACK);  // status
        init_pair(3, COLOR_CYAN, COLOR_BLACK);    // prompt
        // syntax highlighting pairs (4-9)
        init_pair(4, COLOR_BLUE, COLOR_BLACK);    // keyword
        init_pair(5, COLOR_GREEN, COLOR_BLACK);   // string
        init_pair(6, COLOR_WHITE, COLOR_BLACK);   // comment
        init_pair(7, COLOR_YELLOW, COLOR_BLACK);  // number
        init_pair(8, COLOR_MAGENTA, COLOR_BLACK); // type
        init_pair(9, COLOR_CYAN, COLOR_BLACK);    // fence
    }
    getmaxyx(stdscr, _rows, _cols);
    _running = true;
    g_active_repl = this;
    std::atexit(ncurses_atexit_cleanup);
}

void NcursesRepl::teardown() {
    g_active_repl = nullptr;
    if ( _running ) {
        clear();
        refresh();
        endwin();
        // After endwin() the terminal may still show the last alternate-screen
        // frame. Emit a full clear + home escape sequence to leave a clean shell.
        std::cout << "\033[2J\033[H\033[3J" << std::flush;
        _running = false;
    }
    if ( _log_file && _log_file->is_open()) {
        _log_file->close();
    }
    logger::stream[logger::std_out] = &std::cout;
    logger::stream[logger::std_err] = &std::cerr;
}

void NcursesRepl::add_message(const std::string& role, const std::string& text) {
    _history.push_back(role + ":" + text);
    logger::info["ncurses"] << "add_message this=" << this << " role=" << role << " text=[" << text << "] history_size=" << _history.size() << std::endl;
}

static std::vector<std::string> wrap(const std::string& text, size_t width) {
    std::vector<std::string> lines;
    std::string current;
    for ( char c : text ) {
        if ( c == '\n' ) {
            lines.push_back(current);
            current.clear();
        } else {
            current += c;
            if ( current.size() >= width ) {
                lines.push_back(current);
                current.clear();
            }
        }
    }
    if ( !current.empty())
        lines.push_back(current);
    if ( lines.empty())
        lines.push_back("");
    return lines;
}

static std::string box_hline(int cols) {
    // Unicode box-drawing light horizontal: U+2500 (e2 94 80)
    std::string s;
    const char box[] = "\xe2\x94\x80";
    for ( int i = 0; i < cols; ++i )
        s += box;
    return s;
}

// Return the largest byte count <= max_bytes that does not split a UTF-8 character.
static size_t utf8_fit(const std::string& s, size_t max_bytes) {
    if ( max_bytes >= s.size())
        return s.size();
    size_t pos = max_bytes;
    while ( pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80 )
        --pos;
    return pos;
}

// Return the number of terminal columns occupied by the first byte_count bytes.
static int utf8_display_width(const std::string& s, size_t byte_count) {
    int width = 0;
    size_t i = 0;
    while ( i < byte_count && i < s.size()) {
        int char_len = std::mblen(s.c_str() + i, s.size() - i);
        if ( char_len <= 0 ) {
            ++i;
            continue;
        }
        wchar_t wc = 0;
        std::mbstate_t state = {};
        std::mbrtowc(&wc, s.c_str() + i, char_len, &state);
        int w = wcwidth(wc);
        if ( w > 0 )
            width += w;
        i += char_len;
    }
    return width;
}

void NcursesRepl::render_line(int row, const std::string& text, bool is_prompt, Language lang) {
    int x = 1;
    const int max_x = _cols - 2;
    if ( is_prompt ) {
        attron(A_BOLD);
        if ( has_colors()) attron(COLOR_PAIR(3));
    }

    if ( !text.empty() && text.size() >= 3 && text.substr(0, 3) == "```" ) {
        // fence line (opening or closing)
        if ( has_colors()) attron(COLOR_PAIR(_highlighter.color_for_fence()));
        int remaining = max_x - x + 1;
        if ( remaining > 0 )
            mvaddstr(row, x, text.substr(0, utf8_fit(text, remaining)).c_str());
        if ( has_colors()) attroff(COLOR_PAIR(_highlighter.color_for_fence()));
    } else {
        auto spans = _highlighter.highlight(text, lang);
        for ( const auto& span : spans ) {
            if ( x > max_x )
                break;
            if ( span.color_pair != 0 && has_colors()) attron(COLOR_PAIR(span.color_pair));
            if ( span.bold ) attron(A_BOLD);
            int remaining = max_x - x + 1;
            if ( remaining > 0 )
                mvaddstr(row, x, span.text.substr(0, utf8_fit(span.text, remaining)).c_str());
            if ( span.bold ) attroff(A_BOLD);
            if ( span.color_pair != 0 && has_colors()) attroff(COLOR_PAIR(span.color_pair));
            x += utf8_display_width(span.text, span.text.size());
        }
    }

    if ( is_prompt ) {
        if ( has_colors()) attroff(COLOR_PAIR(3));
        attroff(A_BOLD);
    }
}

std::vector<std::tuple<std::string, bool, Language>> NcursesRepl::build_lines(int width) const {
    std::vector<std::tuple<std::string, bool, Language>> out;
    Language lang = Language::none;
    bool in_fence = false;

    auto append_block = [&](const std::string& block, bool is_prompt) {
        size_t start = 0;
        bool first = true;
        while ( start <= block.size()) {
            size_t end = block.find('\n', start);
            if ( end == std::string::npos ) end = block.size();
            std::string line = block.substr(start, end - start);
            start = end + 1;

            if ( line.size() >= 3 && line.substr(0, 3) == "```" ) {
                if ( !in_fence ) {
                    in_fence = true;
                    lang = _highlighter.detect(line.substr(3));
                } else {
                    in_fence = false;
                    lang = Language::none;
                }
                out.emplace_back(line, is_prompt, Language::none);
                first = false;
                continue;
            }

            std::string prefix = is_prompt ? ( first ? "> " : "  " ) : " ";
            auto wrapped = wrap(line, width - (int)prefix.size());
            for ( const auto& w : wrapped ) {
                out.emplace_back(prefix + w, is_prompt, lang);
                if ( is_prompt ) prefix = "  ";
            }
            first = false;
        }
    };

    if ( !_current_reply.empty())
        append_block(_current_reply, false);

    for ( const auto& entry : _history ) {
        size_t pos = entry.find(':');
        if ( pos == std::string::npos )
            continue;
        std::string role = entry.substr(0, pos);
        std::string text = entry.substr(pos + 1);
        append_block(text, role == "prompt");
    }

    return out;
}

void NcursesRepl::draw() {
    {
        std::string dbg = "draw this=" + std::to_string(reinterpret_cast<uintptr_t>(this)) +
                          " _history.size=" + std::to_string(_history.size());
        for ( size_t i = 0; i < _history.size(); ++i )
            dbg += " [" + std::to_string(i) + "]=" + _history[i];
        logger::info["ncurses"] << dbg << std::endl;
    }

    clear();
    getmaxyx(stdscr, _rows, _cols);

    // title row
    std::string left_title = "AI Agent v0.1.0";
    std::string right_title = "Press ctrl-c to exit - ESC to interrupt";
    attron(A_BOLD);
    if ( has_colors()) attron(COLOR_PAIR(1));
    mvaddstr(0, 1, left_title.c_str());
    if ( (int)right_title.size() < _cols - 2 )
        mvaddstr(0, _cols - 1 - (int)right_title.size(), right_title.c_str());
    if ( has_colors()) attroff(COLOR_PAIR(1));
    attroff(A_BOLD);
    mvaddstr(1, 1, box_hline(_cols - 2).c_str());

    // bottom layout (from bottom up):
    // _rows-1 : status bottom (auto mode, cwd, ctx)
    // _rows-2 : status top (status message / settings)
    // _rows-3 : separator
    // _rows-4 : prompt (with side padding)
    // _rows-5 : separator
    // _rows-6 .. _rows-9 : quick-reply suggestions (if terminal is tall enough)
    // _rows-10 : separator above suggestions
    // conversation ends above that

    const int status_bottom_row = _rows - 1;
    const int status_top_row = _rows - 2;
    const int prompt_row = _rows - 4;
    const int prompt_sep_top = _rows - 5;
    const int max_suggestion_rows = 4;
    const int min_total_rows = 11; // title+sep + conv + sugg-sep + 1 sugg + prompt-sep + prompt + status-sep + 2 status
    int suggestion_rows = (_rows >= min_total_rows + max_suggestion_rows - 1) ? max_suggestion_rows
                        : std::max(0, _rows - min_total_rows + 1);
    const int suggestion_bottom_row = prompt_sep_top - 1;
    const int suggestion_top_row = suggestion_bottom_row - suggestion_rows + 1;
    const int suggestion_sep_top = suggestion_top_row - 1;
    int conv_end = suggestion_sep_top - 1;
    if ( suggestion_rows <= 0 )
        conv_end = prompt_sep_top - 1;

    // status top: status message (or settings when idle)
    int sig_count = agent::sigint_count.load(std::memory_order_relaxed);
    std::string top_msg;
    bool top_italic = false;
    bool confirm_active = false;
    {
        std::lock_guard<std::mutex> lock(_confirm_mutex);
        confirm_active = _confirm_pending;
    }
    if ( confirm_active ) {
        top_msg = " Allow: " + _confirm_action + " [y/N]? ";
    } else if ( sig_count >= 1 ) {
        top_msg = " Press Ctrl-C again to exit ";
    } else if ( _abort_current.load(std::memory_order_relaxed)) {
        top_msg = " Aborting... ";
    } else if ( _state == State::processing ) {
        top_italic = true;
        auto elapsed = std::chrono::steady_clock::now() - _animation_start;
        int seconds = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
        top_msg = " AI is thinking... (" + std::to_string(seconds) + "s) ";
    } else {
        top_msg = " " + _config.provider + " | " + _config.model +
                  " | tools:" + (_config.tools_enabled ? "on" : "off") + " ";
    }
    if ( (int)top_msg.size() > _cols - 2 )
        top_msg = top_msg.substr(0, _cols - 5) + "...";
    if ( top_italic )
        attron(A_ITALIC);
    mvaddstr(status_top_row, 1, top_msg.c_str());
    if ( top_italic )
        attroff(A_ITALIC);

    // status bottom: auto mode, working directory, context
    std::string auto_mode = _config.tools_enabled && !_config.confirm_tools ? "auto mode on" : "auto mode off";
    std::string cwd;
    try { cwd = std::filesystem::current_path().string(); } catch ( ... ) { cwd = "?"; }
    std::string left_status = " " + auto_mode + " ";
    std::string center_status = " working directory: " + cwd + " ";

    size_t total_chars = 0;
    for ( const auto& m : _conversation.messages())
        total_chars += m.content.size();
    std::string right_status = " ctx: " + std::to_string(_conversation.messages().size()) +
                               " msgs / " + std::to_string(total_chars) + " chars ";

    mvaddstr(status_bottom_row, 1, left_status.c_str());

    int center_x = (_cols - (int)center_status.size()) / 2;
    if ( center_x < (int)left_status.size() + 2 )
        center_x = (int)left_status.size() + 2;
    if ( center_x + (int)center_status.size() > _cols - 1 - (int)right_status.size() )
        center_status = center_status.substr(0, std::max(0, _cols - 1 - (int)right_status.size() - center_x));
    if ( center_x + (int)center_status.size() <= _cols - 1 )
        mvaddstr(status_bottom_row, center_x, center_status.c_str());

    int ctx_x = _cols - 1 - (int)right_status.size();
    if ( ctx_x < (int)left_status.size() + 2 )
        ctx_x = (int)left_status.size() + 2;
    if ( ctx_x + (int)right_status.size() > _cols - 1 )
        right_status = right_status.substr(0, _cols - 1 - ctx_x);
    if ( ctx_x + (int)right_status.size() <= _cols - 1 )
        mvaddstr(status_bottom_row, ctx_x, right_status.c_str());

    // separator above status top
    mvaddstr(status_top_row - 1, 1, box_hline(_cols - 2).c_str());

    // prompt with side padding
    mvaddstr(prompt_row, 1, "> ");
    int input_width = std::max(0, _cols - 5);
    int visible_len = static_cast<int>(utf8_fit(_input, std::min(input_width, (int)_input.size())));
    std::string visible_input = _input.substr(0, visible_len);
    mvaddstr(prompt_row, 3, visible_input.c_str());

    // fake cursor (hardware cursor is hidden)
    int cursor_x = 3 + utf8_display_width(_input, std::min(_cursor, visible_len));
    if ( cursor_x < _cols - 2 )
        mvaddstr(prompt_row, cursor_x, "[_]");
    else if ( cursor_x < _cols - 1 )
        mvaddstr(prompt_row, cursor_x, "_");

    // separator above prompt
    mvaddstr(prompt_sep_top, 1, box_hline(_cols - 2).c_str());

    // suggestion / pending queue box
    std::string pending_text;
    {
        std::lock_guard<std::mutex> lock(_queue_mutex);
        if ( !_pending_prompts.empty()) {
            std::vector<std::string> items;
            auto tmp = _pending_prompts;
            while ( !tmp.empty()) {
                items.push_back(tmp.front());
                tmp.pop();
            }
            pending_text = "Queued: " + common::join_vector(items, " | ");
        }
    }

    if ( suggestion_rows > 0 ) {
        mvaddstr(suggestion_sep_top, 1, box_hline(_cols - 2).c_str());
        if ( !pending_text.empty()) {
            std::string s = " " + pending_text;
            if ( (int)s.size() > _cols - 2 )
                s = s.substr(0, _cols - 5) + "...";
            mvaddstr(suggestion_top_row, 1, s.c_str());
        } else {
            for ( int i = 0; i < suggestion_rows; ++i ) {
                int row = suggestion_top_row + i;
                if ( i < (int)_suggestions.size()) {
                    std::string s = " " + _suggestions[i];
                    if ( (int)s.size() > _cols - 2 )
                        s = s.substr(0, _cols - 5) + "...";
                    mvaddstr(row, 1, s.c_str());
                }
            }
        }
    }

    int available = conv_end - 2 + 1;
    if ( available < 1 )
        available = 1;

    auto lines = build_lines(_cols - 2);
    logger::info["ncurses"] << "draw lines=" << lines.size() << " available=" << available << " conv_end=" << conv_end << " rows=" << _rows << std::endl;
    int y = 2;
    size_t start = lines.size() > (size_t)available ? lines.size() - available : 0;
    for ( size_t i = start; i < lines.size() && y <= conv_end; i++, y++ ) {
        const auto& [text, is_prompt, lang] = lines[i];
        render_line(y, text, is_prompt, lang);
    }

    // Show the thinking indicator right under the active conversation.
    if ( _state == State::processing && y <= conv_end ) {
        auto elapsed = std::chrono::steady_clock::now() - _animation_start;
        int seconds = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
        std::string thinking = " [...] AI is thinking... (" + std::to_string(seconds) + "s) ";
        if ( (int)thinking.size() > _cols - 2 )
            thinking = thinking.substr(0, _cols - 5) + "...";
        mvaddstr(y, 1, thinking.c_str());
    }

    refresh();
}

void NcursesRepl::process_ui_queue(bool local_change) {
    std::queue<std::function<void()>> updates;
    {
        std::lock_guard<std::mutex> lock(_queue_mutex);
        updates.swap(_ui_queue);
    }
    bool needs_draw = local_change || !updates.empty();
    while ( !updates.empty()) {
        updates.front()();
        updates.pop();
    }
    // Keep the "AI is thinking" animation alive even when no other updates arrive.
    if ( _state == State::processing )
        needs_draw = true;
    // Ensure the "press Ctrl-C again to exit" message is shown after the first signal.
    if ( agent::sigint_count.load(std::memory_order_relaxed) >= 1 )
        needs_draw = true;
    if ( needs_draw )
        draw();
}

bool NcursesRepl::confirm(const std::string& action) {
    {
        std::lock_guard<std::mutex> lock(_confirm_mutex);
        _confirm_action = action;
        _confirm_pending = true;
        _confirm_result = false;
    }
    std::unique_lock<std::mutex> lock(_confirm_mutex);
    _confirm_cv.wait(lock, [this] { return !_confirm_pending; });
    return _confirm_result;
}

void NcursesRepl::submit(const std::string& line) {
    logger::debug["ncurses"] << "submit line=[" << line << "]" << std::endl;
    {
        std::lock_guard<std::mutex> lock(_queue_mutex);
        _pending_prompts.push(line);
    }
    ensure_worker();
}

void NcursesRepl::ensure_worker() {
    bool expected = false;
    if ( !_worker_busy.compare_exchange_strong(expected, true)) {
        logger::debug["ncurses"] << "worker already running" << std::endl;
        return;
    }

    if ( _worker.joinable()) {
        logger::debug["ncurses"] << "joining previous worker" << std::endl;
        _worker.join();
    }

    logger::debug["ncurses"] << "starting worker" << std::endl;
    _worker = std::thread([this]() { worker_loop(); });
}

void NcursesRepl::worker_loop() {
    while ( true ) {
        std::string line;
        {
            std::lock_guard<std::mutex> lock(_queue_mutex);
            if ( _pending_prompts.empty()) {
                _worker_busy = false;
                _state = State::idle;
                _ui_queue.push([this]() { draw(); });
                _queue_cv.notify_one();
                logger::debug["ncurses"] << "worker queue empty, exiting" << std::endl;
                return;
            }
            line = _pending_prompts.front();
            _pending_prompts.pop();
            _state = State::processing;
            _animation_start = std::chrono::steady_clock::now();
            _queue_cv.notify_one();
        }

        logger::debug["ncurses"] << "worker processing line=[" << line << "]" << std::endl;

        _abort_current.store(false, std::memory_order_relaxed);

        try {
            std::string reply = _callback(line, [this](const std::string& chunk) {
                if ( _abort_current.load(std::memory_order_relaxed))
                    return;
                std::lock_guard<std::mutex> lock(_queue_mutex);
                _ui_queue.push([this, chunk]() {
                    _current_reply += chunk;
                });
                _queue_cv.notify_one();
            });

            std::lock_guard<std::mutex> lock(_queue_mutex);
            _ui_queue.push([this, reply]() {
                if ( _abort_current.load(std::memory_order_relaxed)) {
                    _current_reply.clear();
                    return;
                }
                if ( !_current_reply.empty()) {
                    add_message("assistant", _current_reply);
                    _current_reply.clear();
                } else if ( !reply.empty()) {
                    add_message("assistant", reply);
                }
            });
        } catch ( const std::exception& e ) {
            std::lock_guard<std::mutex> lock(_queue_mutex);
            _ui_queue.push([this, msg = std::string(e.what())]() {
                add_message("error", msg);
            });
        }
    }
}

std::string NcursesRepl::read_utf8_char(int first_byte) {
    std::string s(1, static_cast<char>(first_byte));
    int remaining = 0;
    if ( (first_byte & 0xE0) == 0xC0 ) remaining = 1;
    else if ( (first_byte & 0xF0) == 0xE0 ) remaining = 2;
    else if ( (first_byte & 0xF8) == 0xF0 ) remaining = 3;

    for ( int i = 0; i < remaining; ++i ) {
        int b = getch();
        if ( b == ERR || (b & 0xC0) != 0x80 )
            break;
        s += static_cast<char>(b);
    }
    return s;
}

void NcursesRepl::run() {

    setup();

    _cursor = 0;
    draw();

    while ( _running && agent::running.load(std::memory_order_relaxed)) {
        int ch = getch();
        bool local_change = (ch != ERR);

        if ( ch != ERR ) {
            logger::debug["ncurses"] << "key ch=" << ch << std::endl;
            bool confirm_active = false;
            {
                std::lock_guard<std::mutex> lock(_confirm_mutex);
                confirm_active = _confirm_pending;
            }
            if ( confirm_active ) {
                bool result = false;
                if ( ch == 'y' || ch == 'Y' )
                    result = true;
                {
                    std::lock_guard<std::mutex> lock(_confirm_mutex);
                    _confirm_result = result;
                    _confirm_pending = false;
                }
                _confirm_cv.notify_all();
            } else if ( ch == 27 ) { // ESC aborts an active AI request; does not quit
                if ( _worker_busy.load(std::memory_order_relaxed) &&
                     !_abort_current.load(std::memory_order_relaxed)) {
                    _abort_current.store(true, std::memory_order_relaxed);
                    {
                        std::lock_guard<std::mutex> lock(_queue_mutex);
                        while ( !_pending_prompts.empty())
                            _pending_prompts.pop();
                    }
                    add_message("error", "AI request aborted");
                }
            } else if ( ch == 3 ) { // Ctrl-C as raw key
                int count = agent::sigint_count.fetch_add(1, std::memory_order_relaxed) + 1;
                if ( count >= 2 ) {
                    _running = false;
                    break;
                }
            } else if ( ch == '\n' || ch == KEY_ENTER ) {
                std::string line = common::trim_ws(_input);
                logger::debug["ncurses"] << "enter line=[" << line << "] worker_busy=" << _worker_busy.load() << std::endl;
                if ( !line.empty()) {
                    add_message("prompt", line);
                    _prompt_history.push_back(line);
                    if ( line == "/exit" || line == "/quit" ) {
                        break;
                    }
                    submit(line);
                    _history_index = _prompt_history.size();
                }
                _input.clear();
                _cursor = 0;
            } else if ( ch == KEY_BACKSPACE || ch == 127 || ch == '\b' || ch == 8 || ch == 263 ) {
                logger::info["ncurses"] << "backspace ch=" << ch << " (KEY_BACKSPACE=" << KEY_BACKSPACE << ") _cursor=" << _cursor << " input_size=" << _input.size() << std::endl;
                if ( _cursor > 0 ) {
                    // erase one UTF-8 character backwards
                    size_t prev = _cursor - 1;
                    while ( prev > 0 && (static_cast<unsigned char>(_input[prev]) & 0xC0) == 0x80 )
                        --prev;
                    logger::info["ncurses"] << "backspace erase start prev=" << prev << " count=" << (_cursor - prev) << std::endl;
                    _input.erase(prev, _cursor - prev);
                    _cursor = static_cast<int>(prev);
                    logger::info["ncurses"] << "backspace after erase _cursor=" << _cursor << " input_size=" << _input.size() << std::endl;
                } else {
                    logger::info["ncurses"] << "backspace skipped because _cursor <= 0" << std::endl;
                }
            } else if ( ch == KEY_DC ) {
                if ( _cursor < (int)_input.size()) {
                    size_t next = _cursor + 1;
                    while ( next < _input.size() && (static_cast<unsigned char>(_input[next]) & 0xC0) == 0x80 )
                        ++next;
                    _input.erase(_cursor, next - _cursor);
                }
            } else if ( ch == KEY_LEFT ) {
                if ( _cursor > 0 ) {
                    do { --_cursor; } while ( _cursor > 0 && (static_cast<unsigned char>(_input[_cursor]) & 0xC0) == 0x80 );
                }
            } else if ( ch == KEY_RIGHT ) {
                if ( _cursor < (int)_input.size()) {
                    do { ++_cursor; } while ( _cursor < (int)_input.size() && (static_cast<unsigned char>(_input[_cursor]) & 0xC0) == 0x80 );
                }
            } else if ( ch == KEY_HOME ) {
                _cursor = 0;
            } else if ( ch == KEY_END ) {
                _cursor = (int)_input.size();
            } else if ( ch == KEY_UP ) {
                if ( !_prompt_history.empty() && _history_index > 0 ) {
                    _history_index--;
                    _input = _prompt_history[_history_index];
                    _cursor = (int)_input.size();
                }
            } else if ( ch == KEY_DOWN ) {
                if ( !_prompt_history.empty() && _history_index + 1 < _prompt_history.size()) {
                    _history_index++;
                    _input = _prompt_history[_history_index];
                    _cursor = (int)_input.size();
                } else if ( _history_index < _prompt_history.size()) {
                    _history_index = _prompt_history.size();
                    _input.clear();
                    _cursor = 0;
                }
            } else if ( ch == KEY_RESIZE ) {
                // handled by draw()
            } else if ( ch >= 32 && ch < 127 ) { // printable ASCII
                _input.insert(_cursor, 1, static_cast<char>(ch));
                _cursor++;
            } else if ( ch >= 128 && ch < 256 ) { // UTF-8 multibyte start
                std::string utf8 = read_utf8_char(ch);
                _input.insert(_cursor, utf8);
                _cursor += (int)utf8.size();
            } else {
                logger::info["ncurses"] << "unhandled key ch=" << ch << std::endl;
            }
        }

        process_ui_queue(local_change);
    }

    teardown();
    if ( _worker.joinable())
        _worker.join();
}

} // namespace agent
