#pragma once
#include <string>
#include <vector>
#include "pcb.hpp"

namespace ak {

// what the model receives for one turn plus the accounting that proves the budget held
struct Assembled {
    std::string system;
    std::string user;
    std::uint32_t tokens = 0;
    std::uint32_t dropped = 0;
    bool truncated = false;
};

// the model must answer in this exact shape so node can also enforce it with a json schema
inline const char* RESPONSE_CONTRACT =
    "\n\n## response contract\n"
    "reply with one json object only: {\"syscalls\":[...],\"note\":\"one line of reasoning\"}\n"
    "allowed syscalls:\n"
    "{\"call\":\"order\",\"kind\":\"cap|floor|lock\",\"strike_cents\":int,\"gpu_hours\":int,\"limit_premium_cents\":int}\n"
    "{\"call\":\"hold\"}\n"
    "an order is a request, the kernel risk gate may reject it and will tell you why next turn\n";

inline std::string limits_text(const Limits& l) {
    std::string s;
    s += "max_premium_cents " + std::to_string(l.max_premium_cents) + "\n";
    s += "max_gpu_hours " + std::to_string(l.max_gpu_hours) + "\n";
    s += "max_abs_delta_bps " + std::to_string(l.max_abs_delta_bps) + "\n";
    s += "strike_band_bps " + std::to_string(l.strike_band_bps) + "\n";
    s += "max_slippage_bps " + std::to_string(l.max_slippage_bps) + "\n";
    s += "max_orders_per_turn " + std::to_string(l.max_orders_per_turn) + "\n";
    return s;
}

inline std::string exposure_text(const Process& p) {
    const Exposure& e = p.exposure;
    std::string s;
    s += "premium_spent_cents " + std::to_string(e.premium_spent_cents) + " of " + std::to_string(p.limits.max_premium_cents) + "\n";
    s += "gpu_hours " + std::to_string(e.gpu_hours) + " of " + std::to_string(p.limits.max_gpu_hours) + "\n";
    s += "net_delta_bps " + std::to_string(e.net_delta_bps) + " cap " + std::to_string(p.limits.max_abs_delta_bps) + "\n";
    s += "orders " + std::to_string(e.orders) + "\n";
    return s;
}

// builds the turn prompt inside a hard token budget, newest information wins when space runs out
inline Assembled assemble(const Process& p) {
    Assembled a;
    // the system frame only changes when limits change so the api can cache it across turns
    a.system = p.system + "\n\n## hard limits\n" + limits_text(p.limits) + RESPONSE_CONTRACT;
    std::string state = "## state\nturn " + std::to_string(p.turns) + "\n" + exposure_text(p);
    // the fixed section scaffolding is reserved first so the final estimate can never exceed the budget
    constexpr std::size_t SCAFFOLD_BYTES = sizeof("## interrupts newest first\nnone\n## memory\n## recent newest first\nnone\n");
    std::uint32_t used = estimate_tokens(a.system.size()) + estimate_tokens(state.size()) + estimate_tokens(SCAFFOLD_BYTES);
    const std::uint32_t budget = p.budget_tokens;
    if (used > budget) a.truncated = true;

    // selection pass keeps an item only if it still fits, counting what had to be dropped
    auto fits = [&](std::size_t bytes) {
        std::uint32_t t = estimate_tokens(bytes + 1);
        if (used + t > budget) {
            ++a.dropped;
            a.truncated = true;
            return false;
        }
        used += t;
        return true;
    };
    std::vector<const Interrupt*> ints;
    for (auto it = p.pending.rbegin(); it != p.pending.rend(); ++it)
        if (fits(it->kind.size() + 1 + it->payload.size())) ints.push_back(&*it);
    const bool memory = !p.working_set.empty() && fits(p.working_set.size());
    std::vector<const std::string*> recent;
    for (std::size_t i = 0; i < p.recent.size(); ++i) {
        const std::string& s = p.recent.newest(i);
        if (fits(s.size())) recent.push_back(&s);
    }

    // render pass writes the sections in a fixed reading order
    std::string u = state;
    u += "## interrupts newest first\n";
    if (ints.empty()) u += "none\n";
    for (const Interrupt* i : ints) u += i->kind + " " + i->payload + "\n";
    if (memory) u += "## memory\n" + p.working_set + "\n";
    u += "## recent newest first\n";
    if (recent.empty()) u += "none\n";
    for (const std::string* s : recent) u += *s + "\n";
    a.user = std::move(u);
    a.tokens = estimate_tokens(a.system.size()) + estimate_tokens(a.user.size());
    return a;
}

}
