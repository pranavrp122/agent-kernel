#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
#include "third_party/json.hpp"
#include "pcb.hpp"
#include "scheduler.hpp"

namespace ak {

using json = nlohmann::json;

// per operation latency counters so the benchmark and the dashboard read the same numbers
struct OpStats {
    std::uint64_t count = 0;
    std::uint64_t total_ns = 0;
    std::uint64_t max_ns = 0;
};

// the kernel owns the process table and is the only thing allowed to mutate it
class Kernel {
public:
    json handle(const json& req);
    const std::vector<Process>& processes() const { return procs_; }

private:
    json op_spawn(const json& r);
    json op_interrupt(const json& r);
    json op_schedule(const json& r);
    json op_submit(const json& r);
    json op_gc(const json& r);
    json op_kill(const json& r);
    json op_ps(const json& r);
    json op_stats(const json& r);
    json op_replay(const json& r);
    Process* find(Pid pid);
    void record(const std::string& op, const json& req, const json& res);

    std::vector<Process> procs_;
    TenantClock tenant_vruntime_;
    std::uint64_t seq_ = 0;
    std::deque<json> replay_;
    std::unordered_map<std::string, OpStats> stats_;
};

}
