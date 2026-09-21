#include "agent/signal_handler.hpp"

#include <cstdlib>
#include <csignal>
#include <ctime>
#include <unistd.h>
#include "agent/repl_inline.hpp"
#include "signal.hpp"

namespace agent {

std::atomic<bool> running{true};
std::atomic<int> sigint_count{0};
std::atomic<bool> turn_active{false};
std::atomic<bool> turn_abort{false};
std::atomic<bool> turn_steer_interrupt{false};
std::atomic<bool> winch_pending{false};
static std::atomic<int64_t> last_sigint_ms{0};

static void winch_handler(int) {
    winch_pending.store(true, std::memory_order_relaxed);
}

static void signal_handler(int signum) {
    if ( signum == SIGTERM ) {
        int count = sigint_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if ( count >= 2 ) {
            agent::InlineRepl::emergency_teardown();
            std::_Exit(1);
        }
        running.store(false, std::memory_order_relaxed);
        return;
    }

    if ( signum == SIGINT ) {
        // While a turn is running, the first Ctrl-C aborts just that request and
        // keeps the REPL alive.
        if ( turn_active.load(std::memory_order_relaxed)) {
            turn_abort.store(true, std::memory_order_relaxed);
            return;
        }

        int count = sigint_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if ( count >= 3 ) {
            // Force exit if spammed repeatedly while hung.
            agent::InlineRepl::emergency_teardown();
            std::_Exit(1);
        }

        struct timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now_ms = static_cast<int64_t>(ts.tv_sec) * 1000 + (ts.tv_nsec / 1000000);
        int64_t prev = last_sigint_ms.exchange(now_ms, std::memory_order_relaxed);

        if ( prev > 0 && (now_ms - prev) <= 2500 ) {
            // Second signal while idle within 2.5s: ask the main loop to shut down cleanly.
            running.store(false, std::memory_order_relaxed);
        } else {
            // First signal while idle: warn user to confirm exit.
            const char msg[] = "\n● Press Ctrl-C again to exit\n";
            (void)::write(STDERR_FILENO, msg, sizeof(msg) - 1);
        }
        return;
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
