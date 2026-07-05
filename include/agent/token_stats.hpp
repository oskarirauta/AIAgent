#pragma once

#include <atomic>

namespace agent {

// Token accounting shared between the worker thread (which parses `usage` from
// provider responses) and the main thread (which shows it on the status line).
struct TokenStats {
    std::atomic<long> context_tokens{0};  // input/prompt tokens of the latest request (current context size)
    std::atomic<long> session_input{0};   // cumulative input tokens this session
    std::atomic<long> session_output{0};  // cumulative output tokens this session

    long session_total() const {
        return session_input.load(std::memory_order_relaxed) +
               session_output.load(std::memory_order_relaxed);
    }

    void record(long input, long output) {
        if ( input > 0 )
            context_tokens.store(input, std::memory_order_relaxed);
        if ( input > 0 )
            session_input.fetch_add(input, std::memory_order_relaxed);
        if ( output > 0 )
            session_output.fetch_add(output, std::memory_order_relaxed);
    }
};

} // namespace agent
