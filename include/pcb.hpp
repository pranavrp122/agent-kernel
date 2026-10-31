#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ring.hpp"
#include "types.hpp"

namespace ak {

// hard limits a tenant sets when the process is spawned and the kernel never lets it cross
struct Limits {
    Cents max_premium_cents = 0;
    std::int64_t max_gpu_hours = 0;
    DeltaBps max_abs_delta_bps = 0;
    std::int64_t strike_band_bps = 0;
    std::int64_t max_slippage_bps = 0;
    std::uint32_t max_orders_per_turn = 0;
};

// running totals the risk gate checks every order against
struct Exposure {
    Cents premium_spent_cents = 0;
    std::int64_t gpu_hours = 0;
    DeltaBps net_delta_bps = 0;
    std::uint32_t orders = 0;
};

// token and money meter so every process can be billed and throttled
struct Usage {
    std::uint64_t prompt_tokens = 0;
    std::uint64_t cached_tokens = 0;
    std::uint64_t completion_tokens = 0;
    std::uint64_t microcents = 0;
};

// one event delivered to a process, payload is kept as a compact json string
struct Interrupt {
    std::uint64_t seq = 0;
    std::string kind;
    std::string payload;
};

// the process control block, everything the kernel knows about one agent
struct Process {
    Pid pid = 0;
    std::string tenant;
    std::string name;
    std::string system;
    State state = State::Blocked;
    std::uint32_t weight = 1;
    std::uint64_t vruntime = 0;
    std::uint32_t budget_tokens = 0;
    Limits limits;
    Exposure exposure;
    Usage usage;
    std::string working_set;
    Ring<std::string, RECENT_CAPACITY> recent;
    std::vector<std::string> evicted;
    std::vector<Interrupt> pending;
    std::uint32_t turns = 0;
    std::uint32_t last_context_tokens = 0;
    std::uint32_t last_dropped = 0;

    // appends to recent and parks anything the ring pushed out until the summarizer runs
    void remember(std::string entry) {
        if (auto out = recent.push(std::move(entry))) evicted.push_back(std::move(*out));
    }

    bool needs_gc() const { return evicted.size() >= GC_THRESHOLD; }
};

}
