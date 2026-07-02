#include "signal_handler.hpp"

#include <string>
#include "signal.hpp"
#include "logger.hpp"

namespace agent {

std::atomic<bool> running{true};

static void signal_handler(int signum) {
    logger::info["signal"] << "received " << SIG::to_string(signum) << ", shutting down" << std::endl;
    running.store(false, std::memory_order_relaxed);
}

void install_signal_handlers() {
    SIG handler = {
        .TERM = signal_handler,
        .INT = signal_handler,
        .PIPE = [](int){ /* ignore broken pipe */ }
    };
    handler.install();
}

} // namespace agent
