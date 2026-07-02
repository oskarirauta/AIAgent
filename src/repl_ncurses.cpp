#include "repl_ncurses.hpp"

#include <ncurses.h>
#include <cctype>
#include <algorithm>
#include "common.hpp"

namespace agent {

NcursesRepl::NcursesRepl(callback_t cb) : _callback(std::move(cb)) {}

NcursesRepl::~NcursesRepl() {
    teardown();
}

void NcursesRepl::setup() {
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
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
    mvprintw(0, 0, "AI Agent — type 'exit' or press ESC to quit");
    if ( has_colors()) attroff(COLOR_PAIR(1));
    attroff(A_BOLD);
    mvprintw(1, 0, std::string(_cols, '-').c_str());

    int y = 2;
    int available = _rows - y - 2;

    // collect rendered lines from history
    std::vector<std::pair<std::string, bool>> rendered; // text, is_prompt
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

void NcursesRepl::run() {

    setup();

    int cursor = 0;
    draw();

    while ( _running ) {
        int ch = getch();

        if ( ch == 27 ) { // ESC
            break;
        } else if ( ch == '\n' || ch == KEY_ENTER ) {
            std::string line = common::trim_ws(_input);
            if ( !line.empty()) {
                add_message("prompt", line);
                _prompt_history.push_back(line);
                if ( line == "exit" || line == "quit" ) {
                    break;
                }
                std::string reply = _callback(line);
                add_message("assistant", reply);
                _history_index = _prompt_history.size();
            }
            _input.clear();
            cursor = 0;
        } else if ( ch == KEY_BACKSPACE || ch == 127 || ch == '\b' ) {
            if ( cursor > 0 ) {
                _input.erase(cursor - 1, 1);
                cursor--;
            }
        } else if ( ch == KEY_DC ) {
            if ( cursor < (int)_input.size()) {
                _input.erase(cursor, 1);
            }
        } else if ( ch == KEY_LEFT ) {
            if ( cursor > 0 ) cursor--;
        } else if ( ch == KEY_RIGHT ) {
            if ( cursor < (int)_input.size()) cursor++;
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
        } else if ( std::isprint(ch)) {
            _input.insert(cursor, 1, (char)ch);
            cursor++;
        }

        draw();
        move(_rows - 1, 2 + cursor);
        refresh();
    }

    teardown();
}

} // namespace agent
