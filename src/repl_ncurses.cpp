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
    teardown();
    if ( _worker.joinable())
        _worker.join();
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
    mvprintw(0, 0, "AI Agent - type 'exit' or press ESC to quit");
    if ( has_colors()) attroff(COLOR_PAIR(1));
    attroff(A_BOLD);
    mvprintw(1, 0, std::string(_cols, '-').c_str());

    int y = 2;
    int available = _rows - y - 2;

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
        } else {
            auto lines = wrap(text, _cols - 2);
            for ( const auto& l : lines )
                rendered.push_back({ " " + l, false });
        }
    }

    // show last N lines that fit
    size_t start = rendered.size() > (size_t)available ? rendered.size() - available : 0;
    for ( size_t i = start; i < rendered.size() && y < _rows - 2; i++, y++ ) {
        if ( rendered[i].second ) {
            attron(A_BOLD);
            if ( has_colors()) attron(COLOR_PAIR(3));
            mvprintw(y, 0, rendered[i].first.c_str());
            if ( has_colors()) attroff(COLOR_PAIR(3));
            attroff(A_BOLD);
        } else {
            mvprintw(y, 0, rendered[i].first.c_str());
        }
    }

    // prompt line
    mvprintw(_rows - 1, 0, "> ");
    mvprintw(_rows - 1, 2, _input.c_str());
    move(_rows - 1, 2 + (int)_input.size());

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
    logger::debug["ncurses"] << "submit start" << std::endl;
    _worker_busy = true;
    _current_reply.clear();
    if ( _worker.joinable()) {
        logger::debug["ncurses"] << "joining previous worker" << std::endl;
        _worker.join();
    }

    _worker = std::thread([this, line]() {
        try {
            std::string reply = _callback(line, [this](const std::string& chunk) {
                std::lock_guard<std::mutex> lock(_queue_mutex);
                _ui_queue.push([this, chunk]() {
                    _current_reply += chunk;
                    draw();
                });
                _queue_cv.notify_one();
            });

            std::lock_guard<std::mutex> lock(_queue_mutex);
            _ui_queue.push([this, reply]() {
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
        _worker_busy = false;
        logger::debug["ncurses"] << "submit worker finished" << std::endl;
        _queue_cv.notify_one();
    });
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
                break;
            } else if ( ch == '\n' || ch == KEY_ENTER ) {
                std::string line = common::trim_ws(_input);
                logger::debug["ncurses"] << "enter line=[" << line << "] worker_busy=" << _worker_busy.load() << std::endl;
                if ( _worker_busy ) {
                    add_message("error", "please wait for the current response to finish");
                } else {
                    if ( !line.empty()) {
                        add_message("prompt", line);
                        _prompt_history.push_back(line);
                        if ( line == "exit" || line == "quit" ) {
                            break;
                        }
                        submit(line);
                        _history_index = _prompt_history.size();
                    }
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
