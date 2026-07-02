#pragma once

#include <atomic>

namespace agent {

extern std::atomic<bool> running;

void install_signal_handlers();

} // namespace agent
