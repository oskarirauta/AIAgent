#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "agent/syntax_highlighter.hpp"

namespace agent {

class Config;
class Conversation;

class NcursesRepl {
public:
    using callback_t = std::function<std::string(const std::string&, std::function<void(const std::string&)>)>;

    NcursesRepl(callback_t cb, const Config& config, const Conversation& conversation);
    ~NcursesRepl();

    void run();
    bool confirm(const std::string& action);
    void teardown();

private:
    enum class State {
        idle,
        processing
    };

    void setup();
    void draw();
    void add_message(const std::string& role, const std::string& text);
    void render_line(int row, const std::string& text, bool is_prompt, Language lang);
    std::vector<std::tuple<std::string, bool, Language>> build_lines(int width) const;
    void process_ui_queue(bool local_change = false);
    void submit(const std::string& line);
    void ensure_worker();
    void worker_loop();
    static std::string read_utf8_char(int first_byte);
    static int read_escape_sequence(int first_byte);

    callback_t _callback;
    std::string _input;
    std::string _current_reply;
    std::vector<std::string> _history;        // all displayed lines as "role:text"
    std::vector<std::string> _prompt_history; // only user inputs
    std::vector<std::string> _suggestions;    // quick-reply suggestions shown above prompt
    size_t _history_index = 0;
    bool _running = false;
    int _rows = 0;
    int _cols = 0;
    int _cursor = 0;
    int _scroll_offset = 0; // conversation scroll: 0 = bottom, >0 = lines up

    State _state = State::idle;
    std::chrono::steady_clock::time_point _animation_start = std::chrono::steady_clock::now();
    std::atomic<bool> _abort_current{false};
    SyntaxHighlighter _highlighter{4}; // color pairs 4-9 reserved for syntax highlighting
    std::unique_ptr<std::ofstream> _log_file;
    const Config& _config;
    const Conversation& _conversation;

    // Worker thread for the blocking LLM calls.
    std::thread _worker;
    std::mutex _queue_mutex;
    std::condition_variable _queue_cv;
    std::queue<std::function<void()>> _ui_queue;
    std::queue<std::string> _pending_prompts;
    std::atomic<bool> _worker_busy{false};

    // Inline confirmation dialog state.
    std::mutex _confirm_mutex;
    std::condition_variable _confirm_cv;
    std::string _confirm_action;
    bool _confirm_pending = false;
    bool _confirm_result = false;
};

} // namespace agent
