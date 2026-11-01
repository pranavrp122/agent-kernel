#include <cstdio>
#include <string>
#include "kernel.hpp"
#include "ring.hpp"

using ak::json;

static int failures = 0;
static int checks = 0;

// a tiny assertion that keeps running after a failure so one run reports every broken case
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                        \
    } while (0)

// limits every test process starts with, generous enough that only the targeted check trips
static json base_limits() {
    return json{{"max_premium_cents", 100000},
                {"max_gpu_hours", 1000},
                {"max_abs_delta_bps", 50000},
                {"strike_band_bps", 2000},
                {"max_slippage_bps", 100},
                {"max_orders_per_turn", 2}};
}

static json spawn(ak::Kernel& k, const std::string& tenant, const std::string& name, unsigned weight = 1, unsigned budget = 1500) {
    return k.handle(json{{"op", "spawn"}, {"tenant", tenant}, {"name", name}, {"system", "you hedge gpu compute cost"},
                         {"weight", weight}, {"budget_tokens", budget}, {"limits", base_limits()}});
}

static json wake(ak::Kernel& k, unsigned pid, const std::string& kind = "clock") {
    return k.handle(json{{"op", "interrupt"}, {"kind", kind}, {"pid", pid}, {"payload", json{{"t", 1}}}});
}

static json order(const std::string& kind, long long strike, long long index, long long hours, long long premium, long long limit, long long delta) {
    return json{{"call", "order"}, {"kind", kind}, {"strike_cents", strike}, {"index_cents", index}, {"gpu_hours", hours},
                {"premium_cents", premium}, {"limit_premium_cents", limit}, {"delta_bps", delta}};
}

static void test_ring() {
    ak::Ring<int, 3> r;
    CHECK(!r.push(1));
    CHECK(!r.push(2));
    CHECK(!r.push(3));
    auto ev = r.push(4);
    CHECK(ev && *ev == 1);
    CHECK(r.size() == 3);
    CHECK(r.newest(0) == 4);
    CHECK(r.newest(2) == 2);
}

static void test_spawn_and_lifecycle() {
    ak::Kernel k;
    json a = spawn(k, "acme", "hedger");
    CHECK(a["ok"] == true && a["pid"] == 1);
    json b = spawn(k, "acme", "speculator");
    CHECK(b["pid"] == 2);
    json bad = k.handle(json{{"op", "spawn"}, {"name", "x"}});
    CHECK(bad["ok"] == false && bad["error"] == "missing_tenant");
    // nothing is ready until an interrupt arrives
    CHECK(k.handle(json{{"op", "schedule"}})["pid"] == 0);
    json w = wake(k, 1);
    CHECK(w["delivered"] == 1);
    json s = k.handle(json{{"op", "schedule"}});
    CHECK(s["pid"] == 1);
    CHECK(s["system"].get<std::string>().find("max_premium_cents 100000") != std::string::npos);
    CHECK(s["user"].get<std::string>().find("clock {\"t\":1}") != std::string::npos);
    // a running process cannot be scheduled twice and the interrupt was consumed
    CHECK(k.handle(json{{"op", "schedule"}})["pid"] == 0);
    json sub = k.handle(json{{"op", "submit"}, {"pid", 1}, {"syscalls", json::array({json{{"call", "hold"}}})}});
    CHECK(sub["ok"] == true && sub["state"] == "blocked");
    // submitting for a process that is not running is refused
    CHECK(k.handle(json{{"op", "submit"}, {"pid", 1}})["error"] == "not_running");
    CHECK(k.handle(json{{"op", "kill"}, {"pid", 1}})["ok"] == true);
    wake(k, 1);
    CHECK(k.handle(json{{"op", "schedule"}})["pid"] == 0);
    CHECK(k.handle(json{{"op", "nope"}})["error"] == "unknown_op");
}

static void test_risk_gate() {
    ak::Kernel k;
    spawn(k, "acme", "hedger");
    wake(k, 1);
    k.handle(json{{"op", "schedule"}});
    json calls = json::array({
        order("cap", 10000, 10000, 100, 40000, 40000, 5000),
        order("cap", 10000, 10000, 100, 70000, 70000, 5000),
        order("swap", 10000, 10000, 1, 1, 1, 1),
        order("floor", 13000, 10000, 1, 1, 1, 1),
        order("cap", 10000, 10000, 1, 1200, 1000, 1),
    });
    json r = k.handle(json{{"op", "submit"}, {"pid", 1}, {"syscalls", calls},
                           {"usage", json{{"prompt_tokens", 500}, {"completion_tokens", 50}, {"microcents", 2750}}}});
    CHECK(r["accepted"].size() == 1 && r["accepted"][0] == 0);
    CHECK(r["rejected"].size() == 4);
    CHECK(r["rejected"][0]["reason"] == "max_premium");
    CHECK(r["rejected"][1]["reason"] == "bad_kind");
    CHECK(r["rejected"][2]["reason"] == "strike_band");
    CHECK(r["rejected"][3]["reason"] == "slippage");
    json ps = k.handle(json{{"op", "ps"}})["procs"][0];
    CHECK(ps["premium_spent_cents"] == 40000);
    CHECK(ps["gpu_hours"] == 100);
    CHECK(ps["net_delta_bps"] == 5000);
    CHECK(ps["orders"] == 1);
    CHECK(ps["microcents"] == 2750);
    // the per turn order cap is enforced on the second turn as well
    wake(k, 1);
    // the refused order is in the next prompt with the numbers the model needs to re-quote
    json s2 = k.handle(json{{"op", "schedule"}});
    CHECK(s2["user"].get<std::string>().find("reason slippage quoted 1200 limit 1000 hours 1") != std::string::npos);
    json calls2 = json::array({
        order("cap", 10000, 10000, 10, 1000, 1000, 100),
        order("cap", 10000, 10000, 10, 1000, 1000, 100),
        order("cap", 10000, 10000, 10, 1000, 1000, 100),
    });
    json r2 = k.handle(json{{"op", "submit"}, {"pid", 1}, {"syscalls", calls2}});
    CHECK(r2["accepted"].size() == 2);
    CHECK(r2["rejected"][0]["reason"] == "orders_per_turn");
}

static void test_fair_share() {
    ak::Kernel k;
    spawn(k, "acme", "a1", 1);
    spawn(k, "acme", "a2", 1);
    spawn(k, "zeta", "z1", 1);
    k.handle(json{{"op", "interrupt"}, {"kind", "clock"}});
    // acme runs first by pid order, then the untouched tenant zeta must run before acme again
    json s1 = k.handle(json{{"op", "schedule"}});
    CHECK(s1["pid"] == 1);
    k.handle(json{{"op", "submit"}, {"pid", 1}, {"usage", json{{"prompt_tokens", 1000}, {"completion_tokens", 0}}}});
    json s2 = k.handle(json{{"op", "schedule"}});
    CHECK(s2["pid"] == 3);
    k.handle(json{{"op", "submit"}, {"pid", 3}, {"usage", json{{"prompt_tokens", 1000}, {"completion_tokens", 0}}}});
    json s3 = k.handle(json{{"op", "schedule"}});
    CHECK(s3["pid"] == 2);
    // a heavier weight is charged less virtual time for the same tokens
    CHECK(ak::charge(1000, 1) == 1000000);
    CHECK(ak::charge(1000, 4) == 250000);
}

static void test_context_budget_stays_flat() {
    ak::Kernel k;
    spawn(k, "acme", "hedger", 1, 300);
    unsigned first = 0;
    unsigned last = 0;
    bool ever_over = false;
    for (int turn = 0; turn < 60; ++turn) {
        k.handle(json{{"op", "interrupt"}, {"kind", "market"}, {"pid", 1},
                      {"payload", json{{"index_cents", 10000 + turn}, {"note", "a fairly long market payload to fill the ring quickly"}}}});
        json s = k.handle(json{{"op", "schedule"}});
        unsigned tokens = s["tokens"].get<unsigned>();
        if (turn == 0) first = tokens;
        last = tokens;
        if (tokens > 300) ever_over = true;
        k.handle(json{{"op", "submit"}, {"pid", 1}, {"syscalls", json::array({json{{"call", "hold"}}})}, {"note", "waiting for a better strike"}});
    }
    CHECK(!ever_over);
    CHECK(first > 0 && last > 0);
    // sixty turns of history must not grow the prompt past the budget
    CHECK(last <= 300);
    json ps = k.handle(json{{"op", "ps"}})["procs"][0];
    CHECK(ps["last_dropped"].get<unsigned>() > 0);
}

static void test_gc_cycle() {
    ak::Kernel k;
    spawn(k, "acme", "hedger");
    bool asked = false;
    json last;
    for (int turn = 0; turn < 30 && !asked; ++turn) {
        wake(k, 1);
        k.handle(json{{"op", "schedule"}});
        last = k.handle(json{{"op", "submit"}, {"pid", 1}, {"syscalls", json::array({json{{"call", "hold"}}})}, {"note", "n"}});
        asked = last["needs_gc"] == true;
    }
    CHECK(asked);
    CHECK(last["evicted"].size() >= ak::GC_THRESHOLD);
    std::string big(ak::WORKING_SET_MAX_BYTES + 500, 'x');
    json g = k.handle(json{{"op", "gc"}, {"pid", 1}, {"working_set", big}});
    CHECK(g["bytes"] == ak::WORKING_SET_MAX_BYTES);
    json ps = k.handle(json{{"op", "ps"}})["procs"][0];
    CHECK(ps["evicted"] == 0);
    CHECK(ps["working_set_bytes"] == ak::WORKING_SET_MAX_BYTES);
}

static void test_replay_and_stats() {
    ak::Kernel k;
    spawn(k, "acme", "hedger");
    wake(k, 1);
    k.handle(json{{"op", "schedule"}});
    k.handle(json{{"op", "submit"}, {"pid", 1}});
    json rp = k.handle(json{{"op", "replay"}});
    CHECK(rp["entries"].size() == 4);
    CHECK(rp["entries"][0]["op"] == "spawn");
    CHECK(!rp["entries"][0]["req"].contains("system"));
    json st = k.handle(json{{"op", "stats"}});
    CHECK(st["ops"]["schedule"]["count"] == 1);
    CHECK(st["live"] == 1);
}

int main() {
    test_ring();
    test_spawn_and_lifecycle();
    test_risk_gate();
    test_fair_share();
    test_context_budget_stays_flat();
    test_gc_cycle();
    test_replay_and_stats();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
