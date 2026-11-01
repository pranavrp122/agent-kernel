#include "kernel.hpp"
#include <chrono>
#include "context.hpp"
#include "risk.hpp"

namespace ak {

namespace {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

json fail(const std::string& reason) {
    return json{{"ok", false}, {"error", reason}};
}

// keeps a short readable record of an interrupt so the agent remembers what woke it
std::string interrupt_memo(const Interrupt& i) {
    std::string memo = "woke by " + i.kind + " " + i.payload;
    if (memo.size() > 160) memo.resize(160);
    return memo;
}

}

Process* Kernel::find(Pid pid) {
    if (pid == 0 || pid > procs_.size()) return nullptr;
    Process& p = procs_[pid - 1];
    return p.state == State::Terminated ? nullptr : &p;
}

json Kernel::handle(const json& req) {
    const std::string op = req.value("op", "");
    const std::uint64_t t0 = now_ns();
    json res;
    if (op == "spawn") res = op_spawn(req);
    else if (op == "interrupt") res = op_interrupt(req);
    else if (op == "schedule") res = op_schedule(req);
    else if (op == "submit") res = op_submit(req);
    else if (op == "gc") res = op_gc(req);
    else if (op == "kill") res = op_kill(req);
    else if (op == "ps") res = op_ps(req);
    else if (op == "stats") res = op_stats(req);
    else if (op == "replay") res = op_replay(req);
    else res = fail("unknown_op");
    const std::uint64_t ns = now_ns() - t0;
    OpStats& s = stats_[op];
    s.count += 1;
    s.total_ns += ns;
    if (ns > s.max_ns) s.max_ns = ns;
    res["ns"] = ns;
    if (req.contains("id")) res["id"] = req["id"];
    // read only ops are not replayed because they change nothing
    if (op != "ps" && op != "stats" && op != "replay") record(op, req, res);
    return res;
}

json Kernel::op_spawn(const json& r) {
    if (procs_.size() >= MAX_PROCS) return fail("table_full");
    Process p;
    p.pid = static_cast<Pid>(procs_.size() + 1);
    p.tenant = r.value("tenant", "");
    p.name = r.value("name", "agent");
    p.system = r.value("system", "");
    p.weight = r.value("weight", 1u);
    p.budget_tokens = r.value("budget_tokens", 1500u);
    if (p.tenant.empty()) return fail("missing_tenant");
    const json l = r.value("limits", json::object());
    p.limits.max_premium_cents = l.value("max_premium_cents", 0LL);
    p.limits.max_gpu_hours = l.value("max_gpu_hours", 0LL);
    p.limits.max_abs_delta_bps = l.value("max_abs_delta_bps", 0LL);
    p.limits.strike_band_bps = l.value("strike_band_bps", 0LL);
    p.limits.max_slippage_bps = l.value("max_slippage_bps", 0LL);
    p.limits.max_orders_per_turn = l.value("max_orders_per_turn", 0u);
    procs_.push_back(std::move(p));
    return json{{"ok", true}, {"pid", procs_.back().pid}};
}

json Kernel::op_interrupt(const json& r) {
    const std::string kind = r.value("kind", "");
    if (kind.empty()) return fail("missing_kind");
    Interrupt i;
    i.seq = ++seq_;
    i.kind = kind;
    i.payload = r.contains("payload") ? r["payload"].dump() : "{}";
    const Pid pid = r.value("pid", 0u);
    const std::string tenant = r.value("tenant", "");
    std::uint32_t delivered = 0;
    // no pid and no tenant means broadcast to every live process
    for (Process& p : procs_) {
        if (p.state == State::Terminated) continue;
        if (pid != 0 && p.pid != pid) continue;
        if (!tenant.empty() && p.tenant != tenant) continue;
        p.pending.push_back(i);
        if (p.state == State::Blocked) p.state = State::Ready;
        ++delivered;
    }
    return json{{"ok", true}, {"seq", i.seq}, {"delivered", delivered}};
}

json Kernel::op_schedule(const json&) {
    Process* p = pick_next(procs_, tenant_vruntime_);
    if (!p) return json{{"ok", true}, {"pid", 0}};
    Assembled a = assemble(*p);
    p->state = State::Running;
    p->last_context_tokens = a.tokens;
    p->last_dropped = a.dropped;
    // interrupts are consumed once rendered into a context, the memo keeps a trace in recent
    for (const Interrupt& i : p->pending) p->remember(interrupt_memo(i));
    p->pending.clear();
    return json{{"ok", true},
                {"pid", p->pid},
                {"tenant", p->tenant},
                {"name", p->name},
                {"system", a.system},
                {"user", a.user},
                {"tokens", a.tokens},
                {"dropped", a.dropped},
                {"truncated", a.truncated}};
}

json Kernel::op_submit(const json& r) {
    Process* p = find(r.value("pid", 0u));
    if (!p) return fail("no_such_pid");
    if (p->state != State::Running) return fail("not_running");
    const json usage = r.value("usage", json::object());
    const std::uint64_t prompt = usage.value("prompt_tokens", 0ULL);
    const std::uint64_t completion = usage.value("completion_tokens", 0ULL);
    p->usage.prompt_tokens += prompt;
    p->usage.cached_tokens += usage.value("cached_tokens", 0ULL);
    p->usage.completion_tokens += completion;
    p->usage.microcents += usage.value("microcents", 0ULL);

    json accepted = json::array();
    json rejected = json::array();
    std::uint32_t orders_this_turn = 0;
    const json calls = r.value("syscalls", json::array());
    for (std::size_t i = 0; i < calls.size(); ++i) {
        const json& c = calls[i];
        const std::string call = c.value("call", "");
        if (call == "hold") {
            p->remember("hold");
            continue;
        }
        if (call != "order") {
            rejected.push_back(json::object({{"i", i}, {"reason", "bad_call"}}));
            continue;
        }
        OrderReq o;
        o.kind = c.value("kind", "");
        o.strike_cents = c.value("strike_cents", 0LL);
        o.index_cents = c.value("index_cents", 0LL);
        o.gpu_hours = c.value("gpu_hours", 0LL);
        o.premium_cents = c.value("premium_cents", 0LL);
        o.limit_premium_cents = c.value("limit_premium_cents", 0LL);
        o.delta_bps = c.value("delta_bps", 0LL);
        const std::string why = check_order(*p, o, orders_this_turn);
        if (!why.empty()) {
            rejected.push_back(json::object({{"i", i}, {"reason", why}}));
            // the numbers go into history so the model can re-quote next turn instead of repeating the same refused order
            p->remember("rejected " + o.kind + " strike " + std::to_string(o.strike_cents) + " reason " + why + " quoted " + std::to_string(o.premium_cents) + " limit " + std::to_string(o.limit_premium_cents) + " hours " + std::to_string(o.gpu_hours));
            continue;
        }
        apply_order(*p, o);
        ++orders_this_turn;
        accepted.push_back(i);
        p->remember("filled " + o.kind + " strike " + std::to_string(o.strike_cents) + " hours " +
                    std::to_string(o.gpu_hours) + " premium " + std::to_string(o.premium_cents));
    }
    std::string note = r.value("note", "");
    if (!note.empty()) {
        if (note.size() > 200) note.resize(200);
        p->remember("note " + note);
    }

    const std::uint64_t cost = charge(prompt + completion, p->weight);
    p->vruntime += cost;
    tenant_vruntime_[p->tenant] += cost;
    p->turns += 1;
    // a process with nothing waiting goes back to sleep until the next interrupt
    p->state = p->pending.empty() ? State::Blocked : State::Ready;

    json res{{"ok", true},
             {"pid", p->pid},
             {"accepted", accepted},
             {"rejected", rejected},
             {"state", state_name(p->state)},
             {"needs_gc", p->needs_gc()}};
    if (p->needs_gc()) {
        res["evicted"] = p->evicted;
        res["working_set"] = p->working_set;
    }
    return res;
}

json Kernel::op_gc(const json& r) {
    Process* p = find(r.value("pid", 0u));
    if (!p) return fail("no_such_pid");
    std::string ws = r.value("working_set", "");
    if (ws.size() > WORKING_SET_MAX_BYTES) ws.resize(WORKING_SET_MAX_BYTES);
    p->working_set = std::move(ws);
    p->evicted.clear();
    return json{{"ok", true}, {"pid", p->pid}, {"bytes", p->working_set.size()}};
}

json Kernel::op_kill(const json& r) {
    Process* p = find(r.value("pid", 0u));
    if (!p) return fail("no_such_pid");
    p->state = State::Terminated;
    p->pending.clear();
    return json{{"ok", true}, {"pid", p->pid}};
}

json Kernel::op_ps(const json&) {
    json rows = json::array();
    for (const Process& p : procs_) {
        rows.push_back(json{{"pid", p.pid},
                            {"tenant", p.tenant},
                            {"name", p.name},
                            {"state", state_name(p.state)},
                            {"weight", p.weight},
                            {"vruntime", p.vruntime},
                            {"turns", p.turns},
                            {"pending", p.pending.size()},
                            {"recent", p.recent.size()},
                            {"evicted", p.evicted.size()},
                            {"working_set_bytes", p.working_set.size()},
                            {"last_context_tokens", p.last_context_tokens},
                            {"last_dropped", p.last_dropped},
                            {"budget_tokens", p.budget_tokens},
                            {"prompt_tokens", p.usage.prompt_tokens},
                            {"cached_tokens", p.usage.cached_tokens},
                            {"completion_tokens", p.usage.completion_tokens},
                            {"microcents", p.usage.microcents},
                            {"premium_spent_cents", p.exposure.premium_spent_cents},
                            {"gpu_hours", p.exposure.gpu_hours},
                            {"net_delta_bps", p.exposure.net_delta_bps},
                            {"orders", p.exposure.orders}});
    }
    return json{{"ok", true}, {"procs", rows}};
}

json Kernel::op_stats(const json&) {
    json ops = json::object();
    for (const auto& [op, s] : stats_) {
        ops[op] = json{{"count", s.count},
                       {"avg_ns", s.count ? s.total_ns / s.count : 0},
                       {"max_ns", s.max_ns}};
    }
    std::size_t live = 0;
    for (const Process& p : procs_) if (p.state != State::Terminated) ++live;
    return json{{"ok", true}, {"ops", ops}, {"live", live}, {"spawned", procs_.size()}, {"interrupts", seq_}, {"replay", replay_.size()}};
}

json Kernel::op_replay(const json& r) {
    const std::size_t limit = r.value("limit", 100u);
    json out = json::array();
    const std::size_t start = replay_.size() > limit ? replay_.size() - limit : 0;
    for (std::size_t i = start; i < replay_.size(); ++i) out.push_back(replay_[i]);
    return json{{"ok", true}, {"entries", out}};
}

void Kernel::record(const std::string& op, const json& req, const json& res) {
    // the prompt text is dropped from the log because it is reproducible from the state
    json slim = req;
    slim.erase("system");
    json entry{{"seq", replay_.size()}, {"op", op}, {"ok", res.value("ok", false)}, {"ns", res.value("ns", 0ULL)}, {"req", slim}};
    if (res.contains("pid")) entry["pid"] = res["pid"];
    if (res.contains("accepted")) entry["accepted"] = res["accepted"];
    if (res.contains("rejected")) entry["rejected"] = res["rejected"];
    replay_.push_back(std::move(entry));
    if (replay_.size() > REPLAY_CAPACITY) replay_.pop_front();
}

}
