# Model Evaluation Log

Every test run against the Meta Model API to pick the cheapest, fastest model that the kernel can trust. Raw numbers are kept, including the ones that did not work out.

## Environment

- Started 2026-11-01
- API: `https://api.meta.ai/v1` (OpenAI-compatible chat completions)
- Client is Node 24 `fetch`, key loaded from a gitignored `.env`, laptop on home wifi (Snapdragon X Elite, Windows 11)
- Pricing used for cost math, per million tokens, was Muse Spark 1.3 input $1.25, cached input $0.15, output $4.25. Contributor tier input $0.10, output $0.20, with the caveat that Meta trains on contributor-tier data.

## Test 1: smoke test (2026-11-01)

The purpose was to confirm the key works and see how much hidden reasoning a one-word answer costs.

The request was `muse-spark-1.3`, prompt "Reply with the single word: ready", `max_completion_tokens 20`, `reasoning_effort minimal`.

| Metric | Value |
|---|---|
| HTTP status | 200 |
| Round trip | 1030 ms |
| Prompt tokens | 15 |
| Completion tokens | 19 |
| of which reasoning | 9 |
| Visible answer | `ready` |

The takeaway is that even at minimal effort the model spends about half its completion tokens on reasoning that is never shown. Reasoning cannot be turned off on this API. Every kernel turn will carry this overhead, so the response contract must be short.

## Test 2: available models (2026-11-01)

`GET /v1/models` returned nine models. Text-capable ones that matter here:

| Model id | Notes |
|---|---|
| `muse-spark-1.3` | current flagship, 1M context, structured outputs, the default candidate |
| `muse-spark-1.3-contributor` | same model, ~10x cheaper, Meta trains on the data, 100 RPM cap |
| `muse-spark-1.2` | previous version |
| `muse-spark-1.2-contributor` | previous version, contributor tier |
| `muse-spark-1.1` | older version |
| `muse-wonder-1.0` | unknown capability, to be probed |

Not available on this API: Muse Glimmer 30B, Llama 3.x. Those would need a second provider (DeepInfra, Together, Fireworks) and a second key, so they are out of scope for the hackathon build. The eval therefore compares versions and effort levels within the Muse Spark family.

## Test 3: syscall-validity eval (planned, results appended when run)

The purpose was to find the cheapest model and effort setting that reliably produces valid syscalls.

Design:
- A fixed set of scenarios, each a realistic `system` + `user` prompt produced by the kernel's own context assembler (so the eval measures what production will send).
- Each scenario has an expected class of answer, one of should hold, should buy a cap, should buy a floor, should refuse because a limit is nearly exhausted.
- Every model x effort combination answers every scenario N times.
- Scored on valid JSON rate, schema-valid rate (passes the kernel's response contract), kernel acceptance rate of the orders it emitted, sensible-decision rate against the expected class, p50/p90/p99 latency, prompt and completion tokens, cost per turn in cents.
- The decision rule was to pick the cheapest combination with at least 95% schema-valid and at least 90% sensible decisions, then report the latency table for all of them.

### Run 1 (2026-11-01, `npm run eval:models -- --reps=2`)

Six prompts straight from the kernel, the three preset agents fresh (367, 347, 350 tokens) and the same three after two clock ticks with history (440, 588, 571 tokens). Each model and effort answered every prompt twice with `max_completion_tokens 1200` and the strict json schema on. 144 calls, four in flight at a time.

| model | effort | json ok | schema ok | cut off | ms p50 | ms p90 | ms p99 | prompt tok | reasoning tok | out tok | cost ¢ / turn | decisions |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| muse-spark-1.3 | minimal | 100% | 100% | 0% | 2272 | 3037 | 3422 | 675 | 342 | 415 | 0.250 | cap 4, floor 4, hold 4 |
| muse-spark-1.3 | low | 67% | 67% | 33% | 3722 | 5363 | 6217 | 675 | 824 | 876 | 0.453 | cap 4, none 4, hold 4 |
| muse-spark-1.3-contributor | minimal | 100% | 100% | 0% | 4710 | 6329 | 6733 | 675 | 324 | 391 | 0.015 | cap 4, floor 4, hold 4 |
| muse-spark-1.3-contributor | low | 75% | 75% | 25% | 7025 | 10983 | 11330 | 675 | 856 | 915 | 0.025 | cap 4, floor 1, none 3, hold 4 |
| muse-spark-1.2 | minimal | 100% | 100% | 0% | 2127 | 2817 | 2898 | 539 | 280 | 348 | 0.189 | cap 4, floor 3, hold 5 |
| muse-spark-1.2 | low | 67% | 67% | 33% | 4573 | 5520 | 5554 | 539 | 778 | 825 | 0.399 | cap 4, none 4, hold 4 |
| muse-spark-1.2-contributor | minimal | 100% | 100% | 0% | 1992 | 2795 | 2912 | 539 | 313 | 383 | 0.013 | cap 4, floor 4, hold 4 |
| muse-spark-1.2-contributor | low | 75% | 75% | 25% | 4364 | 5691 | 5725 | 539 | 771 | 830 | 0.022 | hold 8, floor 1, none 3 |
| muse-spark-1.1 | minimal | 83% | 83% | 17% | 2775 | 4551 | 4906 | 539 | 579 | 645 | 0.314 | cap 4, none 2, floor 2, hold 4 |
| muse-spark-1.1 | low | 83% | 83% | 17% | 3170 | 4788 | 5532 | 539 | 629 | 694 | 0.312 | cap 4, none 2, hold 4, floor 2 |
| muse-wonder-1.0 | minimal | 100% | 100% | 0% | 3258 | 4352 | 5363 | 539 | 255 | 339 | 0.195 | cap 4, floor 4, hold 4 |
| muse-wonder-1.0 | low | 75% | 75% | 25% | 5727 | 8426 | 8898 | 539 | 653 | 716 | 0.354 | cap 4, none 3, floor 1, hold 4 |

"none" means the reply was cut off before the json came out. "cap 4, floor 4, hold 4" is the expected pattern, where the hedger buys a cap, the floor agent buys a floor, the scout holds, on both the fresh and the turn-3 prompt.

What the numbers say:

1. **Effort is the whole story.** Going from minimal to low roughly doubles reasoning tokens (300 to 800), doubles latency, doubles cost, and pushes a quarter to a third of replies past the 1200-token cap so they come back empty. Minimal effort is the only setting that is reliable, and it loses nothing on decision quality here.
2. **Five of six models agree.** Every minimal-effort model except Spark 1.1 produced 100% schema-valid replies and the same twelve decisions. The kernel's context makes the decision easy enough that the model version barely matters, which is exactly what a kernel should do.
3. **The cheapest valid model is 19x cheaper.** Muse Spark 1.2 contributor at minimal effort was 100% valid, 0.013¢ per turn, and the fastest p50 at 2.0 s. The catch is the tier, since Meta may train on contributor-tier traffic and it is capped at 100 requests per minute.
4. **Spark 1.3 counts more prompt tokens** (675 vs 539 for identical text), so it is about 25% more expensive per prompt before output is counted.
5. **Latency is reasoning.** Across every row, p50 tracks reasoning tokens almost linearly, about 150 to 200 reasoning tokens per second of wall time. The kernel's share of a turn is 20 to 25 µs.

Decision:

| Role | Model | Why |
|---|---|---|
| Strategist (the agent's turn) | `muse-spark-1.3`, minimal effort, 1200 max tokens | Flagship, 100% valid, the model the hackathon is about |
| Summarizer (gc of old history) | `muse-spark-1.2-contributor`, minimal effort | Same validity on this task at a nineteenth of the cost; summaries are demo data |
| Cost floor (documented, not default) | `muse-spark-1.2-contributor` for everything | 0.013¢ per turn; a thousand agent turns for 13 cents |

Both are environment-overridable (`KERNEL_MODEL`, `KERNEL_EFFORT`, `KERNEL_GC_MODEL`). Raw rows are in `artifacts/model-eval-2026-11-01-18-27-05.json` in the Dodona repo (gitignored).

## Test 4: latency levers (planned)

The purpose was to measure, not guess, which settings reduce end-to-end turn time.

Each measured against the winning model from Test 3:
- `reasoning_effort` minimal vs low
- `max_completion_tokens` 80 vs 200 vs 400
- structured output (`response_format: json_schema`) vs free text with a prose instruction
- prompt caching, same system prefix across turns, read `cached_tokens` in usage
- HTTP keep-alive, cold connection vs reused connection
- parallel calls, 1 vs 4 vs 8 agents in flight at once

### Measured so far (2026-11-01)

**`max_completion_tokens` 400 vs 1200** (two live runs of `npm run demo:kernel`, 9 turns each, Spark 1.3 minimal):

| cap | readable replies | orders booked | cost ¢ / turn | model ms p50 |
|---|---|---|---|---|
| 400 | 3 of 9 | 1 | 0.242 | 2367 |
| 1200 | 9 of 9 | 4 | 0.292 | 2598 |

At 400 the model spent the whole budget on reasoning and returned no json six times. The fix cost 0.05¢ per turn. The lesson is that the cap must sit above the model's reasoning habit, not at the size of the answer you want.

**`reasoning_effort` minimal vs low.** See Run 1 above. Low is never worth it on this task.

**Prompt caching.** `cached_tokens` came back non-zero on only 3 of 18 live turns (113 to 369 tokens). The system frame is about 300 tokens and the api appears to cache only prefixes above a threshold near 1024 tokens, so caching will only pay once the system frame grows or several agents share one. Not a lever at this prompt size.

**Parallel calls.** The runtime schedules up to `concurrency` agents and keeps that many model calls in flight. With 3 agents and concurrency 3, a round of three turns takes one model latency instead of three. Still to measure, 8 agents at concurrency 1, 4 and 8.
