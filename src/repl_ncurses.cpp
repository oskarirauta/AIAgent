#include "agent/repl_ncurses.hpp"

#include <ncurses.h>
#include <cctype>
#include <algorithm>
#include "common.hpp"
#include "logger.hpp"
#include "agent/signal_handler.hpp"

namespace agent {

NcursesRepl::NcursesRepl(callback_t cb) : _callback(std::move(cb)) {}

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
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    timeout(50); // non-blocking input so the UI can update while the worker runs
    if ( has_colors()) {
        start_color();
        init_pair(1, COLOR_GREEN, COLOR_BLACK);
        init_pair(2, COLOR_YELLOW, COLOR_BLACK);
        init_pair(3, COLOR_CYAN, COLOR_BLACK);
    }
    getmaxyx(stdscr, _rows, _cols);
    _running = true;
}

void NcursesRepl::teardown() {
    if ( _running ) {
        endwin();
        _running = false;
    }
}

void NcursesRepl::add_message(const std::string& role, const std::string& text) {
    _history.push_back(role + ":" + text);
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

void NcursesRepl::draw() {
    clear();
    getmaxyx(stdscr, _rows, _cols);

    attron(A_BOLD);
    if ( has_colors()) attron(COLOR_PAIR(1));
    mvprintw(0, 0, "AI Agent - ESC aborts AI, Ctrl-C quits");
    if ( has_colors()) attroff(COLOR_PAIR(1));
    attroff(A_BOLD);
    mvprintw(1, 0, std::string(_cols, '-').c_str());

    // bottom layout (from bottom up):
    // _rows-1 : info bar
    // _rows-2 : separator
    // _rows-3 : prompt (with side padding)
    // _rows-4 : separator
    // if pending queue:
    //   _rows-5 : queued messages
    //   _rows-6 : separator
    // conversation ends above that

    const int info_row = _rows - 1;
    const int prompt_row = _rows - 3;
    const int prompt_sep_top = _rows - 4;
    int conv_end = prompt_sep_top - 1;

    // info bar
    std::string info;
    int sig_count = agent::sigint_count.load(std::memory_order_relaxed);
    if ( sig_count >= 1 )
        info = " Press Ctrl-C again to exit ";
    else
        info = " ESC=abort AI  Ctrl-C=quit ";
    if ( (int)info.size() > _cols )
        info = info.substr(0, _cols);
    mvaddstr(info_row, 0, info.c_str());
    mvprintw(info_row - 1, 0, std::string(_cols, '-').c_str());

    // prompt with side padding
    mvaddstr(prompt_row, 1, "> ");
    mvaddstr(prompt_row, 3, _input.c_str());
    move(prompt_row, 3 + (int)_input.size());

    // separator above prompt
    mvprintw(prompt_sep_top, 0, std::string(_cols, '-').c_str());

    // pending queue (if any)
    bool has_pending = false;
    std::string pending_text;
    {
        std::lock_guard<std::mutex> lock(_queue_mutex);
        has_pending = !_pending_prompts.empty();
        if ( has_pending ) {
            std::vector<std::string> items;
            auto tmp = _pending_prompts;
            while ( !tmp.empty()) {
                items.push_back(tmp.front());
                tmp.pop();
            }
            pending_text = "Queued: " + common::join_vector(items, " | ");
        }
    }

    if ( has_pending ) {
        const int queue_text_row = _rows - 5;
        const int queue_sep_top = _rows - 6;
        mvprintw(queue_sep_top, 0, std::string(_cols, '-').c_str());
        if ( (int)pending_text.size() > _cols - 2 )
            pending_text = pending_text.substr(0, _cols - 5) + "...";
        mvaddstr(queue_text_row, 1, pending_text.c_str());
        conv_end = queue_sep_top - 1;
    }

    int available = conv_end - 2 + 1;
    if ( available < 1 )
        available = 1;

    // collect rendered lines from history + current streaming reply
    std::vector<std::pair<std::string, bool>> rendered; // text, is_prompt

    if ( !_current_reply.empty()) {
        auto lines = wrap(_current_reply, _cols - 2);
        for ( const auto& l : lines )
            rendered.push_back({ " " + l, false });
    }

    for ( const auto& entry : _history ) {
        size_t pos = entry.find(':');
        if ( pos == std::string::npos )
            continue;
        std::string role = entry.substr(0, pos);
        std::string text = entry.substr(pos + 1);

        if ( role == "prompt" ) {
            rendered.push_back({ "> " + text, true });
        } else if ( role == "error" ) {
            auto lines = wrap(text, _cols - 2);
            for ( const auto& l : lines )
                rendered.push_back({ " " + l, false });
        } else if ( role == "assistant" ) {
            auto lines = wrap(text, _cols - 2);
            for ( const auto& l : lines )
                rendered.push_back({ " " + l, false });
        } else {
            auto lines = wrap(text, _cols - 2);
            for ( const auto& l : lines )
                rendered.push_back({ " " + l, false });
        }
    }

    // show last N lines that fit
    int y = 2;
    size_t start = rendered.size() > (size_t)available ? rendered.size() - available : 0;
    for ( size_t i = start; i < rendered.size() && y <= conv_end; i++, y++ ) {
        if ( rendered[i].second ) {
            attron(A_BOLD);
            if ( has_colors()) attron(COLOR_PAIR(3));
            mvaddstr(y, 0, rendered[i].first.c_str());
            if ( has_colors()) attroff(COLOR_PAIR(3));
            attroff(A_BOLD);
        } else {
            mvaddstr(y, 0, rendered[i].first.c_str());
        }
    }

    // status line inside conversation area
    if ( _state == State::processing && y <= conv_end ) {
        attron(A_BOLD);
        if ( has_colors()) attron(COLOR_PAIR(2));
        mvaddstr(y, 1, "AI is thinking...");
        if ( has_colors()) attroff(COLOR_PAIR(2));
        attroff(A_BOLD);
    }

    refresh();
}

void NcursesRepl::process_ui_queue() {
    std::queue<std::function<void()>> updates;
    {
        std::lock_guard<std::mutex> lock(_queue_mutex);
        updates.swap(_ui_queue);
    }
    while ( !updates.empty()) {
        updates.front()();
        updates.pop();
        move(_rows - 1, 2 + (int)_input.size());
        refresh();
    }
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
            _ui_queue.push([this]() { draw(); });
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
                    draw();
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

    int cursor = 0;
    draw();

    while ( _running && agent::running.load(std::memory_order_relaxed)) {
        int ch = getch();

        if ( ch != ERR ) {
            if ( ch == 27 ) { // ESC
                if ( _worker_busy.load(std::memory_order_relaxed)) {
                    _abort_current.store(true, std::memory_order_relaxed);
                    {
                        std::lock_guard<std::mutex> lock(_queue_mutex);
                        while ( !_pending_prompts.empty())
                            _pending_prompts.pop();
                    }
                    add_message("error", "AI request aborted");
                } else {
                    break;
                }
            } else if ( ch == '\n' || ch == KEY_ENTER ) {
                std::string line = common::trim_ws(_input);
                logger::debug["ncurses"] << "enter line=[" << line << "] worker_busy=" << _worker_busy.load() << std::endl;
                if ( !line.empty()) {
                    add_message("prompt", line);
                    _prompt_history.push_back(line);
                    if ( line == "exit" || line == "quit" ) {
                        break;
                    }
                    submit(line);
                    _history_index = _prompt_history.size();
                }
                _input.clear();
                cursor = 0;
            } else if ( ch == KEY_BACKSPACE || ch == 127 || ch == '\b' ) {
                logger::debug["ncurses"] << "backspace ch=" << ch << " cursor=" << cursor << " input_size=" << _input.size() << std::endl;
                if ( cursor > 0 ) {
                    // erase one UTF-8 character backwards
                    size_t prev = cursor;
                    while ( prev > 0 && (static_cast<unsigned char>(_input[prev - 1]) & 0xC0) == 0x80 )
                        --prev;
                    _input.erase(prev, cursor - prev);
                    cursor = static_cast<int>(prev);
                }
            } else if ( ch == KEY_DC ) {
                if ( cursor < (int)_input.size()) {
                    size_t next = cursor + 1;
                    while ( next < _input.size() && (static_cast<unsigned char>(_input[next]) & 0xC0) == 0x80 )
                        ++next;
                    _input.erase(cursor, next - cursor);
                }
            } else if ( ch == KEY_LEFT ) {
                if ( cursor > 0 ) {
                    do { --cursor; } while ( cursor > 0 && (static_cast<unsigned char>(_input[cursor]) & 0xC0) == 0x80 );
                }
            } else if ( ch == KEY_RIGHT ) {
                if ( cursor < (int)_input.size()) {
                    do { ++cursor; } while ( cursor < (int)_input.size() && (static_cast<unsigned char>(_input[cursor]) & 0xC0) == 0x80 );
                }
            } else if ( ch == KEY_HOME ) {
                cursor = 0;
            } else if ( ch == KEY_END ) {
                cursor = (int)_input.size();
            } else if ( ch == KEY_UP ) {
                if ( !_prompt_history.empty() && _history_index > 0 ) {
                    _history_index--;
                    _input = _prompt_history[_history_index];
                    cursor = (int)_input.size();
                }
            } else if ( ch == KEY_DOWN ) {
                if ( !_prompt_history.empty() && _history_index + 1 < _prompt_history.size()) {
                    _history_index++;
                    _input = _prompt_history[_history_index];
                    cursor = (int)_input.size();
                } else if ( _history_index < _prompt_history.size()) {
                    _history_index = _prompt_history.size();
                    _input.clear();
                    cursor = 0;
                }
            } else if ( ch == KEY_RESIZE ) {
                // handled by draw()
            } else if ( ch >= 32 && ch < 127 ) { // printable ASCII
                _input.insert(cursor, 1, static_cast<char>(ch));
                cursor++;
            } else if ( ch >= 128 && ch < 256 ) { // UTF-8 multibyte start
                std::string utf8 = read_utf8_char(ch);
                _input.insert(cursor, utf8);
                cursor += (int)utf8.size();
            }

            draw();
            move(_rows - 1, 2 + cursor);
            refresh();
        }

        process_ui_queue();
    }

    teardown();
    if ( _worker.joinable())
        _worker.join();
}

} // namespace agent
