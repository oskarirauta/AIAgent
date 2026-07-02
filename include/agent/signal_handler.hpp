#pragma once

#include <atomic>

namespace agent {

extern std::atomic<bool> running;
extern std::atomic<int> sigint_count;

void install_signal_handlers();

} // namespace agent
