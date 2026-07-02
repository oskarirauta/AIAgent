#include "agent/signal_handler.hpp"

#include <string>
#include "signal.hpp"
#include "logger.hpp"

namespace agent {

std::atomic<bool> running{true};
std::atomic<int> sigint_count{0};

static void signal_handler(int signum) {
    if ( signum == SIGINT || signum == SIGTERM ) {
        int count = sigint_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if ( count >= 2 ) {
            logger::warning["signal"] << "received second " << SIG::to_string(signum) << ", exiting immediately" << std::endl;
            std::exit(1);
        }
        logger::info["signal"] << "received " << SIG::to_string(signum) << ", press again to force quit" << std::endl;
    } else {
        // ignore broken pipe
    }
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
