#pragma once

#include <atomic>
#include <mutex>

namespace agent {

struct TurnUsage {
    size_t turn_number = 0;
    size_t model_requests = 0;
    size_t tool_calls = 0;
    long input_tokens = 0;
    long cached_tokens = 0;
    long output_tokens = 0;
    long reasoning_tokens = 0;
    long first_request_tokens = 0;
    long peak_request_tokens = 0;
    long elapsed_ms = 0;
};

// Token accounting shared between the worker thread (which parses `usage` from
// provider responses) and the main thread (which shows it on the status line).
struct TokenStats {
    std::atomic<long> context_tokens{0};  // input/prompt tokens of the latest request (current context size)
    std::atomic<long> session_input{0};   // cumulative input tokens this session
    std::atomic<long> session_output{0};  // cumulative output tokens this session
    std::atomic<long> session_cached{0};  // cumulative cache-read input tokens (a subset of input)
    std::atomic<long> last_cached{0};      // cache-read input tokens of the latest request

    mutable std::mutex turn_mx;
    TurnUsage last_turn;

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

    void record_turn(const TurnUsage& tu) {
        std::lock_guard<std::mutex> lk(turn_mx);
        last_turn = tu;
    }

    TurnUsage get_last_turn() const {
        std::lock_guard<std::mutex> lk(turn_mx);
        return last_turn;
    }
};

} // namespace agent
