# Context benchmark: a common trader agent with and without the kernel

Run on 2026-11-01 against the Meta Model API, model `muse-spark-1.3` at minimal effort, on the Dodona demo platform's own pricing, order book and clock. Everything here is reproducible with `bench/context/bench.mjs`; the raw per-day records are in `bench/context/results-*.json` and the console logs in `bench/context/run-*.log`.

## 1. What the popular trader agents do with context

| Project | Stars (2026-11) | Loop | Where the history lives | Risk limits |
|---|---|---|---|---|
| [TradingAgents](https://github.com/TauricResearch/TradingAgents) (Tauric Research) | ~119k | LangGraph pipeline, one run per ticker per day: analysts, bull/bear debate, trader, risk debate, portfolio manager | Inside a run, a LangGraph `MessagesState` that every agent appends to. Across days, a memory log on disk; each run loads at most 5 same-ticker and 3 cross-ticker reflections (`get_past_context(n_same=5, n_cross=3)` in `tradingagents/memory/log.py`) | Qualitative: aggressive, conservative and neutral debators argue, the portfolio manager decides. No numeric caps in the agent |
| [ai-hedge-fund](https://github.com/virattt/ai-hedge-fund) (virattt) | ~64k | One run per rebalance date through analyst personas, then portfolio construction | Fresh state per run; paper-trading ledger on disk | Numeric, but as a clamp after the decision (`hedge_fund/risk/limits.py` scales weights to `max_position_pct`, `max_gross_exposure`), not a gate the agent is told about per order |
| [FinMem](https://arxiv.org/abs/2311.13743) (Yu et al.) | paper | One decision per day | Layered long-term memory, top-K retrieval by recency, relevance and importance | None in the agent |
| Every LangGraph or OpenAI Agents SDK "trading bot" tutorial | n/a | A ReAct agent on one thread with a checkpointer, called once per tick | The thread's message list, which grows forever | Whatever the prompt says |

The two big repos are per-decision pipelines for listed equities. Their data tools (price history, fundamentals, news, sentiment) are tied to stock tickers, so they cannot be pointed at a GPU-options order book without rewriting them. What they share with every other agent on the list, and what I could run against the platform unchanged, is the framework loop underneath: LangGraph's prebuilt ReAct agent (`createReactAgent`) with a `MemorySaver` checkpointer on one thread. That loop is the "without our platform" arm below.

## 2. Method

Three arms, same model, same seeded platform, same 30 simulated days, same mandate and the same six hard limits (the `hedger` preset: H100 US East, 7-day windows starting 14 days out, max premium $4,000, max 4,000 GPU-hours, max 2 orders per day, 15% strike band, 3% slippage).

| Arm | What runs each day | Memory | Risk gate |
|---|---|---|---|
| `naive` | LangGraph `createReactAgent` on one thread. Tools: `get_quote`, `place_order`, `get_exposure`. The day's market snapshot is appended as a human message | the full thread, every message since day 1 | none: `place_order` books straight into the order book. The limits are in the system prompt in the same words the kernel uses |
| `window` | the same agent with a `preModelHook` that trims the model's input to the last 10 messages (`trimMessages`, LangChain's standard sliding window) | last 10 messages | none |
| `kernel` | the production runtime: `tick` then `run`, one scheduler turn per day | the kernel's three tiers: 16-entry ring, parked evictions, helper-model working set, under a 1,400-token budget | the kernel's eight integer checks before anything is booked |

The framework arms get `get_exposure` so they can look up what they hold, which is more than most tutorials give an agent. Their prompts carry the same market snapshot the kernel delivers as an interrupt. Prices and bookings go through the same `quoteFromHistory`, `createOrder` and `recordPaid` calls in both cases, so a given order costs the same premium in every arm. Orders the framework arms booked were checked after the fact against the kernel's limits and counted as "unsafe" if the kernel would have refused them.

Token counts come from the API's `usage` field. Cost uses the published Muse Spark 1.3 prices ($1.25 per million input, $0.15 cached, $4.25 output).

## 3. Results

| | naive LangGraph agent | windowed LangGraph agent | kernel |
|---|---|---|---|
| model calls over 30 days | 96 | 94 | 52 (30 turns + 22 helper summaries) |
| largest prompt, day 1 → day 30 | 1,554 → **16,075** tokens | 1,553 → 1,942 | 557 → 1,146 |
| prompt tokens sent in total | 845,446 (92% served from the provider's cache) | 153,136 | 33,019 |
| completion tokens | 8,872 | 13,902 | 11,412 |
| cost at list price with caching | 23.97¢ | 16.76¢ | **8.45¢** |
| cost if the provider did not cache | ~109¢ | ~25¢ | ~9¢ |
| median day latency (all calls that day) | 2.26 s | 2.87 s | 2.66 s |
| orders booked | 23 | 20 | 23 |
| GPU-hours covered at the end | 3,864 of 4,000 | 3,864 | 3,864 |
| premium spent | $451.92 | $470.40 | $393.12 |
| orders the kernel would have refused | 0 | 0 | 7 refused before booking |

Per-day series are in `results-all-arms.json` (naive, window, and the first kernel run) and `results-kernel-rerun.json` (the kernel run reported above).

### What the numbers say

**Context.** The naive agent's prompt grows by about 500 tokens a day and reaches 16k tokens by day 30, with no end in sight; a 90-day run would pass 45k. The windowed agent stays flat at about 1.9k but only because LangChain throws the oldest messages away. The kernel stays flat at about 1.1k and keeps the old history in a compressed working set the helper model writes; it never sent more than 839 kernel-estimated tokens of its own context, and the budget line was never hit (`dropped` was 0 every day; evictions came from the 16-entry ring, which is the intended path).

**Cost.** The kernel run cost a third of the naive run and half of the windowed run. The naive run is only 2.8× rather than 25× more expensive because Meta's API served 92% of its prompt tokens from cache; the "no cache" row is what the same transcript would cost on a provider without prefix caching, or with a prompt that changes near the top.

**Outcome.** All three arms ended at the same 3,864 GPU-hours of coverage, which is the most the mandate allows with 168-hour lots. The kernel's agent paid the least premium, but across a single run of a volatile series that is timing, not a claim. The framework agents booked nothing the kernel would have refused: with a generous mandate, an exposure tool and the limits spelled out in the prompt, the model stayed inside them on its own for 30 days. The kernel's gate fired seven times: six `max_gpu_hours` refusals on days 25 to 30, where the model kept proposing one more 168-hour lot that would have taken it to 4,032 hours, and one slippage refusal on day 23.

**Latency.** Per-day latency is similar everywhere because the model call dominates. The framework arms make about three calls a day (think, call a tool, answer); the kernel makes one strict-JSON call and, on 22 of the 30 days, one cheap summary call to the contributor-tier model.

### What the benchmark found in the kernel

The first 30-day kernel run booked one order and was refused 29 days in a row for slippage (`results-all-arms.json`, arm `kernel`). The cause was the kernel's own feedback: the history line it wrote for a refused order was `rejected cap strike 0 reason slippage`, with no numbers, so the model kept re-submitting the same limit price it had anchored on day 1 while the live cap premium climbed from 18¢ to 32¢ per hour. The fix is in `src/kernel.cpp`: the remembered line now carries the quoted premium, the agent's limit and the size (`… reason slippage quoted 2352 limit 1848 hours 168`), with a test in `tests/test_kernel.cpp` that the next prompt contains it. On the re-run the model re-quoted the next day and went on to book 23 orders. The naive agent did not have this problem because `place_order` has no limit price at all: it books whatever the market quotes, which is exactly the behavior the gate exists to prevent.

## 4. Caveats

- One run per arm, one model, one 30-day price path. The direction of the token and cost results is structural (one list grows, one is trimmed, one is paged); the premium figures are not.
- The framework arms were given more help than a typical tutorial bot: an exposure tool and the limits in the kernel's own words. Without those, the only thing standing between the model and the order book is the prompt.
- TradingAgents and ai-hedge-fund themselves were not run. Their loop was, with the platform's tools in place of their equity data tools. Both are one-shot-per-decision designs, so their per-day context does not grow the way the thread agent's does; it is instead several thousand tokens of analyst reports re-sent every day, which is a different cost problem the kernel does not address.

## 5. Reproduce

```sh
cd bench/context
npm install                       # @langchain/langgraph, @langchain/openai, @langchain/core, zod
DODONA_LIB=file:///path/to/dodona/lib/ KERNEL_BIN=/path/to/build/kernel.exe MODEL_API_KEY=... \
  node bench.mjs --days=30 --arms=kernel,naive,window --window=10
```

The script prints one line per day per arm and a summary table, and writes `results-<timestamp>.json`. The platform code is imported from the Dodona checkout on `feature/agent-kernel`, so the pricer, the order book and the clock are the production paths.

Sources for section 1: the two repositories linked above (read on 2026-11-01, including `tradingagents/graph/propagation.py`, `tradingagents/agents/state.py`, `tradingagents/memory/log.py`, `hedge_fund/risk/limits.py`, `hedge_fund/backtesting/fund.py`); [FinMem, Yu et al. 2023](https://arxiv.org/abs/2311.13743); [Agentic Trading: When LLM Agents Meet Financial Markets, 2026](https://arxiv.org/abs/2605.19337); [LLM Agent in Financial Trading: A Survey, Ding et al.](https://arxiv.org/abs/2408.06361); star counts from GitHub on 2026-11-01.
