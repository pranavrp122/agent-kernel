#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace ak {

// money is always integer cents so float rounding can never leak into a risk check
using Cents = std::int64_t;
// delta is kept in basis points of one gpu-hour so 10000 means fully long one unit
using DeltaBps = std::int64_t;
using Pid = std::uint32_t;

enum class State : std::uint8_t { Ready, Running, Blocked, Terminated };

inline const char* state_name(State s) {
    switch (s) {
        case State::Ready: return "ready";
        case State::Running: return "running";
        case State::Blocked: return "blocked";
        case State::Terminated: return "terminated";
    }
    return "unknown";
}

// four bytes per token is the standard rough estimate for english text and json
constexpr std::size_t BYTES_PER_TOKEN = 4;

inline std::uint32_t estimate_tokens(std::size_t bytes) {
    return static_cast<std::uint32_t>((bytes + BYTES_PER_TOKEN - 1) / BYTES_PER_TOKEN);
}

// hard ceiling on live processes so the table is a fixed allocation
constexpr std::size_t MAX_PROCS = 256;
// how many recent entries a process keeps before the oldest is evicted for gc
constexpr std::size_t RECENT_CAPACITY = 16;
// evicted entries that pile up before the kernel asks the summarizer to compress them
constexpr std::size_t GC_THRESHOLD = 8;
// longest working set summary the kernel will store, in bytes
constexpr std::size_t WORKING_SET_MAX_BYTES = 1600;
// replay log keeps this many most recent operations
constexpr std::size_t REPLAY_CAPACITY = 10000;

}
