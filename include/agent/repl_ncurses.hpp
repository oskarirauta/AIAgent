#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
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

private:
    enum class State {
        idle,
        processing
    };

    void setup();
    void teardown();
    void draw();
    void add_message(const std::string& role, const std::string& text);
    void render_line(int row, const std::string& text, bool is_prompt, Language lang);
    std::vector<std::tuple<std::string, bool, Language>> build_lines(int width) const;
    void process_ui_queue();
    void submit(const std::string& line);
    void ensure_worker();
    void worker_loop();
    static std::string read_utf8_char(int first_byte);

    callback_t _callback;
    std::string _input;
    std::string _current_reply;
    std::vector<std::string> _history;        // all displayed lines as "role:text"
    std::vector<std::string> _prompt_history; // only user inputs
    size_t _history_index = 0;
    bool _running = false;
    int _rows = 0;
    int _cols = 0;
    int _cursor = 0;

    State _state = State::idle;
    std::chrono::steady_clock::time_point _animation_start = std::chrono::steady_clock::now();
    std::atomic<bool> _abort_current{false};
    SyntaxHighlighter _highlighter{4}; // color pairs 4-9 reserved for syntax highlighting
    const Config& _config;
    const Conversation& _conversation;

    // Worker thread for the blocking LLM calls.
    std::thread _worker;
    std::mutex _queue_mutex;
    std::condition_variable _queue_cv;
    std::queue<std::function<void()>> _ui_queue;
    std::queue<std::string> _pending_prompts;
    std::atomic<bool> _worker_busy{false};
};

} // namespace agent
