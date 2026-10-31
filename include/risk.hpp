#pragma once
#include <cstdlib>
#include <string>
#include "pcb.hpp"

namespace ak {

// one order after node has priced it, every field is an integer so the checks are exact
struct OrderReq {
    std::string kind;
    Cents strike_cents = 0;
    Cents index_cents = 0;
    std::int64_t gpu_hours = 0;
    Cents premium_cents = 0;
    Cents limit_premium_cents = 0;
    DeltaBps delta_bps = 0;
};

// returns an empty string when the order passes, otherwise the reason code the model sees next turn
inline std::string check_order(const Process& p, const OrderReq& o, std::uint32_t orders_this_turn) {
    const Limits& l = p.limits;
    const Exposure& e = p.exposure;
    if (o.kind != "cap" && o.kind != "floor" && o.kind != "lock") return "bad_kind";
    if (o.gpu_hours <= 0 || o.strike_cents <= 0 || o.index_cents <= 0 || o.premium_cents < 0) return "nonpositive";
    if (l.max_orders_per_turn && orders_this_turn >= l.max_orders_per_turn) return "orders_per_turn";
    if (e.premium_spent_cents + o.premium_cents > l.max_premium_cents) return "max_premium";
    if (e.gpu_hours + o.gpu_hours > l.max_gpu_hours) return "max_gpu_hours";
    if (std::llabs(e.net_delta_bps + o.delta_bps) > l.max_abs_delta_bps) return "max_delta";
    // distance of the strike from the index in basis points, all integer math
    const std::int64_t band = std::llabs(o.strike_cents - o.index_cents) * 10000 / o.index_cents;
    if (band > l.strike_band_bps) return "strike_band";
    // the live quote may exceed the agent's own limit price only by the slippage allowance
    if (o.limit_premium_cents > 0 && o.premium_cents * 10000 > o.limit_premium_cents * (10000 + l.max_slippage_bps)) return "slippage";
    return "";
}

// commits an accepted order into the running exposure totals
inline void apply_order(Process& p, const OrderReq& o) {
    p.exposure.premium_spent_cents += o.premium_cents;
    p.exposure.gpu_hours += o.gpu_hours;
    p.exposure.net_delta_bps += o.delta_bps;
    p.exposure.orders += 1;
}

}
