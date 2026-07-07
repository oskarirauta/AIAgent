#pragma once

#include <atomic>

namespace agent {

// Token accounting shared between the worker thread (which parses `usage` from
// provider responses) and the main thread (which shows it on the status line).
struct TokenStats {
    std::atomic<long> context_tokens{0};  // input/prompt tokens of the latest request (current context size)
    std::atomic<long> session_input{0};   // cumulative input tokens this session
    std::atomic<long> session_output{0};  // cumulative output tokens this session
    std::atomic<long> session_cached{0};  // cumulative cache-read input tokens (a subset of input)
    std::atomic<long> last_cached{0};      // cache-read input tokens of the latest request

    long session_total() const {
        return session_input.load(std::memory_order_relaxed) +
               session_output.load(std::memory_order_relaxed);
    }

    void record(long input, long output, long cached = 0) {
        if ( input > 0 ) {
            context_tokens.store(input, std::memory_order_relaxed);
            session_input.fetch_add(input, std::memory_order_relaxed);
        }
        if ( output > 0 )
            session_output.fetch_add(output, std::memory_order_relaxed);
        last_cached.store(cached > 0 ? cached : 0, std::memory_order_relaxed);
        if ( cached > 0 )
            session_cached.fetch_add(cached, std::memory_order_relaxed);
    }
};

} // namespace agent
