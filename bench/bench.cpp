#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include "context.hpp"
#include "kernel.hpp"
#include "risk.hpp"

using ak::json;

static std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// percentiles over raw samples so tail latency is reported, not just the mean
static void report(const char* name, std::vector<std::uint64_t> s) {
    std::sort(s.begin(), s.end());
    const auto at = [&](double q) { return s[static_cast<std::size_t>(q * (s.size() - 1))]; };
    std::printf("| %-28s | %8llu | %8llu | %8llu | %8llu |\n", name,
                static_cast<unsigned long long>(at(0.50)), static_cast<unsigned long long>(at(0.90)),
                static_cast<unsigned long long>(at(0.99)), static_cast<unsigned long long>(s.back()));
}

int main() {
    constexpr int PROCS = 64;
    constexpr int TENANTS = 8;
    constexpr int ITERS = 20000;
    ak::Kernel k;
    json limits{{"max_premium_cents", 1000000000}, {"max_gpu_hours", 100000000}, {"max_abs_delta_bps", 1000000000},
                {"strike_band_bps", 2000}, {"max_slippage_bps", 100}, {"max_orders_per_turn", 4}};
    for (int i = 0; i < PROCS; ++i) {
        k.handle(json{{"op", "spawn"}, {"tenant", "tenant" + std::to_string(i % TENANTS)}, {"name", "agent" + std::to_string(i)},
                      {"system", "you manage gpu compute options for one customer and keep cost under the cap"},
                      {"weight", 1 + (i % 3)}, {"budget_tokens", 1200}, {"limits", limits}});
    }
    // warm every process with a realistic history so context assembly has real work to do
    json market{{"index_cents", 24150}, {"vol_bps", 6500}, {"window", "2026-11-10/2026-11-17"}, {"capacity_hours", 5000}};
    for (int round = 0; round < 20; ++round) {
        k.handle(json{{"op", "interrupt"}, {"kind", "market"}, {"payload", market}});
        for (int i = 0; i < PROCS; ++i) {
            json s = k.handle(json{{"op", "schedule"}});
            if (s["pid"] == 0) break;
            k.handle(json{{"op", "submit"}, {"pid", s["pid"]}, {"note", "holding until vol settles"},
                          {"syscalls", json::array({json{{"call", "order"}, {"kind", "cap"}, {"strike_cents", 24500}, {"index_cents", 24150},
                                                          {"gpu_hours", 10}, {"premium_cents", 3200}, {"limit_premium_cents", 3200}, {"delta_bps", 4200}}})},
                          {"usage", json{{"prompt_tokens", 900}, {"completion_tokens", 60}}}});
        }
    }

    std::vector<std::uint64_t> t_interrupt, t_schedule, t_submit, t_pick, t_assemble, t_risk;
    json order_calls = json::array({
        json{{"call", "order"}, {"kind", "cap"}, {"strike_cents", 24500}, {"index_cents", 24150}, {"gpu_hours", 10}, {"premium_cents", 3200}, {"limit_premium_cents", 3200}, {"delta_bps", 4200}},
        json{{"call", "order"}, {"kind", "floor"}, {"strike_cents", 23800}, {"index_cents", 24150}, {"gpu_hours", 10}, {"premium_cents", 2100}, {"limit_premium_cents", 2100}, {"delta_bps", -3100}},
    });
    json usage{{"prompt_tokens", 900}, {"completion_tokens", 60}, {"microcents", 138000}};
    for (int it = 0; it < ITERS; ++it) {
        const unsigned pid = 1 + (it % PROCS);
        std::uint64_t t0 = now_ns();
        k.handle(json{{"op", "interrupt"}, {"kind", "market"}, {"pid", pid}, {"payload", market}});
        std::uint64_t t1 = now_ns();
        json s = k.handle(json{{"op", "schedule"}});
        std::uint64_t t2 = now_ns();
        json r = k.handle(json{{"op", "submit"}, {"pid", s["pid"]}, {"syscalls", order_calls}, {"usage", usage}, {"note", "rolled the cap up"}});
        std::uint64_t t3 = now_ns();
        t_interrupt.push_back(t1 - t0);
        t_schedule.push_back(t2 - t1);
        t_submit.push_back(t3 - t2);
        // the driver would summarize here, the bench stands in with a fixed working set
        if (r["needs_gc"] == true) k.handle(json{{"op", "gc"}, {"pid", s["pid"]}, {"working_set", "holds one cap at 24500, rolled twice, vol steady"}});
    }

    // the pure algorithms are timed without json so the cost of the wire format is visible separately
    std::vector<ak::Process> procs;
    for (const ak::Process& p : k.processes()) procs.push_back(p);
    ak::TenantClock clock;
    for (ak::Process& p : procs) {
        p.state = ak::State::Ready;
        clock[p.tenant] += p.vruntime;
    }
    for (int it = 0; it < ITERS; ++it) {
        std::uint64_t t0 = now_ns();
        ak::Process* p = ak::pick_next(procs, clock);
        std::uint64_t t1 = now_ns();
        t_pick.push_back(t1 - t0);
        if (p) p->vruntime += 1;
    }
    ak::Process& sample = procs[0];
    for (int it = 0; it < ITERS; ++it) {
        std::uint64_t t0 = now_ns();
        ak::Assembled a = ak::assemble(sample);
        std::uint64_t t1 = now_ns();
        t_assemble.push_back(t1 - t0);
        if (a.tokens == 0) std::printf("unexpected empty context\n");
    }
    ak::OrderReq o{"cap", 24500, 24150, 10, 3200, 3200, 4200};
    for (int it = 0; it < ITERS; ++it) {
        std::uint64_t t0 = now_ns();
        std::string why = ak::check_order(sample, o, 0);
        std::uint64_t t1 = now_ns();
        t_risk.push_back(t1 - t0);
        if (!why.empty()) std::printf("unexpected rejection %s\n", why.c_str());
    }

    std::printf("processes %d tenants %d iterations %d\n\n", PROCS, TENANTS, ITERS);
    std::printf("| %-28s | %8s | %8s | %8s | %8s |\n", "operation (ns)", "p50", "p90", "p99", "max");
    std::printf("|------------------------------|----------|----------|----------|----------|\n");
    report("interrupt (json in/out)", t_interrupt);
    report("schedule (json in/out)", t_schedule);
    report("submit 2 orders (json)", t_submit);
    report("pick_next only", t_pick);
    report("assemble context only", t_assemble);
    report("risk check only", t_risk);
    json st = k.handle(json{{"op", "stats"}});
    std::printf("\nkernel self reported avg ns: schedule %llu submit %llu interrupt %llu\n",
                st["ops"]["schedule"]["avg_ns"].get<unsigned long long>(), st["ops"]["submit"]["avg_ns"].get<unsigned long long>(),
                st["ops"]["interrupt"]["avg_ns"].get<unsigned long long>());
    std::printf("context tokens on sample process after warmup: %u (budget 1200)\n", ak::assemble(sample).tokens);
    return 0;
}
