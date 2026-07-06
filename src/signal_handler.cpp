#include "agent/signal_handler.hpp"

#include <cstdlib>
#include <csignal>
#include "agent/repl_inline.hpp"
#include "signal.hpp"

namespace agent {

std::atomic<bool> running{true};
std::atomic<int> sigint_count{0};
std::atomic<bool> turn_active{false};
std::atomic<bool> turn_abort{false};
std::atomic<bool> winch_pending{false};

static void winch_handler(int) {
    winch_pending.store(true, std::memory_order_relaxed);
}

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

    // SIGWINCH is not part of the SIG helper; install it directly. No SA_RESTART,
    // so a resize interrupts the REPL's select() and the redraw happens promptly.
    struct sigaction sa {};
    sa.sa_handler = winch_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, nullptr);
}

} // namespace agent
