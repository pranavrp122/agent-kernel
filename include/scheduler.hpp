#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "pcb.hpp"

namespace ak {

using TenantClock = std::unordered_map<std::string, std::uint64_t>;

// two level fair share, the tenant that has consumed the least runs first then its hungriest process
inline Process* pick_next(std::vector<Process>& procs, const TenantClock& tenant_vruntime) {
    Process* best = nullptr;
    std::uint64_t best_tenant = 0;
    std::uint64_t best_proc = 0;
    for (Process& p : procs) {
        if (p.state != State::Ready) continue;
        auto it = tenant_vruntime.find(p.tenant);
        const std::uint64_t t = it == tenant_vruntime.end() ? 0 : it->second;
        if (!best || t < best_tenant || (t == best_tenant && p.vruntime < best_proc)) {
            best = &p;
            best_tenant = t;
            best_proc = p.vruntime;
        }
    }
    return best;
}

// a heavier weight consumes virtual time more slowly so it gets scheduled more often
inline std::uint64_t charge(std::uint64_t tokens, std::uint32_t weight) {
    if (tokens == 0) tokens = 1;
    return tokens * 1000 / (weight == 0 ? 1 : weight);
}

}
