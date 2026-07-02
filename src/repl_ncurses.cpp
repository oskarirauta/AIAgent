#include "agent/repl_ncurses.hpp"

#include <ncurses.h>
#include <cctype>
#include <algorithm>
#include "common.hpp"
#include "logger.hpp"
#include "agent/config.hpp"
#include "agent/conversation.hpp"
#include "agent/signal_handler.hpp"

namespace agent {

NcursesRepl::NcursesRepl(callback_t cb, const Config& config, const Conversation& conversation)
    : _callback(std::move(cb)), _config(config), _conversation(conversation) {}

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

void NcursesRepl::render_line(int row, const std::string& text, bool is_prompt, Language lang) {
    int x = 0;
    if ( is_prompt ) {
        attron(A_BOLD);
        if ( has_colors()) attron(COLOR_PAIR(3));
    }

    if ( !text.empty() && text.size() >= 3 && text.substr(0, 3) == "```" ) {
        // fence line (opening or closing)
        if ( has_colors()) attron(COLOR_PAIR(_highlighter.color_for_fence()));
        mvaddnstr(row, x, text.c_str(), _cols);
        if ( has_colors()) attroff(COLOR_PAIR(_highlighter.color_for_fence()));
    } else {
        auto spans = _highlighter.highlight(text, lang);
        for ( const auto& span : spans ) {
            if ( x >= _cols )
                break;
            if ( span.color_pair != 0 && has_colors()) attron(COLOR_PAIR(span.color_pair));
            if ( span.bold ) attron(A_BOLD);
            int remaining = _cols - x;
            if ( remaining > 0 )
                mvaddnstr(row, x, span.text.c_str(), remaining);
            if ( span.bold ) attroff(A_BOLD);
            if ( span.color_pair != 0 && has_colors()) attroff(COLOR_PAIR(span.color_pair));
            x += (int)span.text.size();
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
    clear();
    getmaxyx(stdscr, _rows, _cols);

    attron(A_BOLD);
    if ( has_colors()) attron(COLOR_PAIR(1));
    mvprintw(0, 0, "AI Agent - ESC aborts AI, Ctrl-C quits");
    if ( has_colors()) attroff(COLOR_PAIR(1));
    attroff(A_BOLD);
    mvprintw(1, 0, std::string(_cols, '-').c_str());

    // bottom layout (from bottom up):
    // _rows-1 : status bottom (left messages, right context usage)
    // _rows-2 : status top (settings)
    // _rows-3 : separator
    // _rows-4 : prompt (with side padding)
    // _rows-5 : separator
    // if pending queue:
    //   _rows-6 : queued messages
    //   _rows-7 : separator
    // conversation ends above that

    const int status_bottom_row = _rows - 1;
    const int status_top_row = _rows - 2;
    const int prompt_row = _rows - 4;
    const int prompt_sep_top = _rows - 5;
    int conv_end = prompt_sep_top - 1;

    // status top: settings
    std::string settings = " " + _config.provider + " | " + _config.model +
                           " | tools:" + (_config.tools_enabled ? "on" : "off") +
                           " | confirm:" + (_config.confirm_tools ? "on" : "off") + " ";
    if ( (int)settings.size() > _cols )
        settings = settings.substr(0, _cols - 3) + "...";
    mvaddstr(status_top_row, 0, settings.c_str());

    // status bottom: left messages + right context
    int sig_count = agent::sigint_count.load(std::memory_order_relaxed);
    std::string left_msg;
    if ( sig_count >= 1 )
        left_msg = " Press Ctrl-C again to exit ";
    else if ( _abort_current.load(std::memory_order_relaxed))
        left_msg = " Aborting... ";
    else if ( _state == State::processing )
        left_msg = " AI is thinking... ";
    else
        left_msg = " Ready ";

    size_t total_chars = 0;
    for ( const auto& m : _conversation.messages())
        total_chars += m.content.size();
    std::string right_ctx = " ctx: " + std::to_string(_conversation.messages().size()) +
                            " msgs / " + std::to_string(total_chars) + " chars ";

    if ( (int)left_msg.size() > _cols )
        left_msg = left_msg.substr(0, _cols);
    mvaddstr(status_bottom_row, 0, left_msg.c_str());

    int ctx_x = _cols - (int)right_ctx.size();
    if ( ctx_x < (int)left_msg.size() + 2 )
        ctx_x = (int)left_msg.size() + 2;
    if ( ctx_x + (int)right_ctx.size() > _cols )
        right_ctx = right_ctx.substr(0, _cols - ctx_x);
    if ( ctx_x < _cols )
        mvaddstr(status_bottom_row, ctx_x, right_ctx.c_str());

    mvprintw(status_bottom_row - 1, 0, std::string(_cols, '-').c_str());

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
        const int queue_text_row = _rows - 6;
        const int queue_sep_top = _rows - 7;
        mvprintw(queue_sep_top, 0, std::string(_cols, '-').c_str());
        if ( (int)pending_text.size() > _cols - 2 )
            pending_text = pending_text.substr(0, _cols - 5) + "...";
        mvaddstr(queue_text_row, 1, pending_text.c_str());
        conv_end = queue_sep_top - 1;
    }

    int available = conv_end - 2 + 1;
    if ( available < 1 )
        available = 1;

    auto lines = build_lines(_cols - 2);
    int y = 2;
    size_t start = lines.size() > (size_t)available ? lines.size() - available : 0;
    for ( size_t i = start; i < lines.size() && y <= conv_end; i++, y++ ) {
        const auto& [text, is_prompt, lang] = lines[i];
        render_line(y, text, is_prompt, lang);
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
