#include "agent/signal_handler.hpp"

#include <cstdlib>
#include "agent/repl_inline.hpp"
#include "signal.hpp"

namespace agent {

std::atomic<bool> running{true};
std::atomic<int> sigint_count{0};
std::atomic<bool> turn_active{false};
std::atomic<bool> turn_abort{false};

static void signal_handler(int signum) {
    if ( signum == SIGINT || signum == SIGTERM ) {
        // While a turn is running, the first Ctrl-C aborts just that request and
        // keeps the REPL alive.
        if ( turn_active.load(std::memory_order_relaxed)) {
            turn_abort.store(true, std::memory_order_relaxed);
            return;
        }

        int count = sigint_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if ( count >= 2 ) {
            // Second signal while idle: restore the terminal and terminate now.
            agent::InlineRepl::emergency_teardown();
            std::_Exit(1);
        }
        // First signal while idle: ask the main loop to shut down cleanly.
        running.store(false, std::memory_order_relaxed);
    }
    // Everything else (e.g. broken pipe) is ignored.
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
