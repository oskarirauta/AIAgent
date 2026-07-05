#pragma once

#include <atomic>

namespace agent {

extern std::atomic<bool> running;
extern std::atomic<int> sigint_count;

// A turn (LLM request) is in progress. While true, SIGINT aborts just that turn
// (via turn_abort) instead of quitting the program.
extern std::atomic<bool> turn_active;
extern std::atomic<bool> turn_abort;

void install_signal_handlers();

} // namespace agent
