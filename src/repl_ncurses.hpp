#pragma once

#include <string>
#include <functional>
#include <vector>

namespace agent {

class NcursesRepl {
public:
    using callback_t = std::function<std::string(const std::string&)>;

    explicit NcursesRepl(callback_t cb);
    ~NcursesRepl();

    void run();

private:
    void setup();
    void teardown();
    void draw();
    void add_message(const std::string& role, const std::string& text);

    callback_t _callback;
    std::string _input;
    std::vector<std::string> _history;        // all displayed lines as "role:text"
    std::vector<std::string> _prompt_history; // only user inputs
    size_t _history_index = 0;
    bool _running = false;
    int _rows = 0;
    int _cols = 0;
};

} // namespace agent
