/** Context benchmark: the same 30-day market stream through a LangGraph ReAct trader (full history, then windowed) and through the agent kernel, same model, same platform. */
import { writeFileSync } from 'node:fs';
import { ChatOpenAI } from '@langchain/openai';
import { tool } from '@langchain/core/tools';
import { HumanMessage, trimMessages } from '@langchain/core/messages';
import { createReactAgent } from '@langchain/langgraph/prebuilt';
import { MemorySaver } from '@langchain/langgraph';
import { z } from 'zod';

// the platform is imported straight from the dodona checkout so pricing, booking and the clock are the production code paths
const LIB = process.env.DODONA_LIB;
if (!LIB) throw new Error('set DODONA_LIB to the file:// url of the dodona platform lib/ folder, with a trailing slash');
const { openDatabase, get, transaction } = await import(LIB + 'db.mjs');
const { seedPlatform } = await import(LIB + 'seed.mjs');
const { fallbackHistory } = await import(LIB + 'prices.mjs');
const { createKernelClient } = await import(LIB + 'kernel-client.mjs');
const { createMuseClient, costMicrocents, MUSE_BASE_URL, DEFAULT_MODEL } = await import(LIB + 'muse.mjs');
const { createAgentRuntime, PRESETS, black76Delta } = await import(LIB + 'agent-runtime.mjs');
const { quoteFromHistory, ALLOWED_COUNTS } = await import(LIB + 'pricing.mjs');
const { accountPrices } = await import(LIB + 'prints.mjs');
const { createOrder, QUOTE_TYPE } = await import(LIB + 'orders.mjs');
const { recordPaid } = await import(LIB + 'payments.mjs');
const { advance } = await import(LIB + 'clock.mjs');

// flags look like --days=30 --arms=kernel,naive,window --window=10 --model=muse-spark-1.3
const args = Object.fromEntries(process.argv.slice(2).map(a => a.replace(/^--/, '').split('=')));
const DAYS = Number(args.days ?? 30);
const ARMS = String(args.arms ?? 'kernel,naive,window').split(',');
const WINDOW = Number(args.window ?? 10);
const MODEL = args.model ?? DEFAULT_MODEL;
const DAY_MS = 86400000;
const preset = PRESETS.find(p => p.key === 'hedger');
const cents = n => Math.round(n * 100);
const midnight = iso => Date.parse(iso.slice(0, 10) + 'T00:00:00.000Z');

// one fresh platform per arm so the order books never mix
function platform() {
  const db = openDatabase(':memory:');
  const history = fallbackHistory();
  const nowMs = Date.now();
  seedPlatform(db, { history, nowMs });
  const account = () => get(db, 'SELECT * FROM accounts WHERE email = ?', preset.tenant);
  return { db, history, nowMs, account };
}

// the same market snapshot the kernel delivers as an interrupt, built with the platform's own quote engine
function snapshot(db, account, history) {
  const m = preset.mandate;
  const start = midnight(account.demo_now) + m.leadDays * DAY_MS;
  const window = { start: new Date(start).toISOString(), end: new Date(start + m.windowDays * DAY_MS).toISOString() };
  const seen = accountPrices(db, account, history);
  const base = { gpu: m.gpu, count: 8, ...window, region: m.region, config: m.config };
  const cap = quoteFromHistory({ ...base, type: 'call' }, seen, { now: account.demo_now });
  const lock = quoteFromHistory({ ...base, type: 'lock' }, seen, { now: account.demo_now });
  return {
    at: account.demo_now.slice(0, 10), gpu: m.gpu, index_cents: cents(cap.spot), forward_cents: cents(cap.forward), vol_bps: Math.round(cap.vol * 10000),
    window: `${window.start.slice(0, 10)}/${window.end.slice(0, 10)}`, atm_strike_cents: cents(cap.strike),
    atm_cap_premium_cents_per_hour: cents(cap.premiumPerGpuHour), lock_cents: cents(lock.strike),
  };
}

// prices one request exactly like the runtime's enrich, so both arms pay the same premium for the same order
function price(db, account, history, { kind, strike_cents, gpu_hours }) {
  const m = preset.mandate;
  const type = QUOTE_TYPE[kind];
  if (!type) throw new Error('kind must be cap, floor or lock');
  const start = midnight(account.demo_now) + m.leadDays * DAY_MS;
  const wanted = (Number(gpu_hours) || 0) / (m.windowDays * 24);
  const count = ALLOWED_COUNTS.reduce((best, c) => (Math.abs(c - wanted) < Math.abs(best - wanted) ? c : best), ALLOWED_COUNTS[0]);
  const atTheMoney = kind === 'lock' || !(Number(strike_cents) > 0);
  const custom = { gpu: m.gpu, count, start: new Date(start).toISOString(), end: new Date(start + m.windowDays * DAY_MS).toISOString(), kind, region: m.region, config: m.config, ...(atTheMoney ? {} : { strike: Number(strike_cents) / 100 }) };
  const quote = quoteFromHistory({ ...custom, type }, accountPrices(db, account, history), { now: account.demo_now });
  const delta = black76Delta({ type, forward: quote.forward, strike: quote.strike, vol: quote.vol, tEffYears: quote.tEffYears, tYears: quote.tYears, rate: quote.inputs.rate });
  return { kind, count, strike_cents: cents(quote.strike), index_cents: cents(quote.forward), gpu_hours: Math.round(quote.gpuHours), premium_cents: cents(quote.premiumTotal), delta_bps: Math.round(delta * quote.gpuHours * 10000), custom: { ...custom, strike: custom.strike ?? quote.strike } };
}

function book(db, account, quoted, history) {
  return transaction(db, () => {
    const order = createOrder(db, account, { custom: quoted.custom }, history);
    recordPaid(db, order, order.pay_now > 0 ? { brand: 'Visa', last4: '4242', at: account.demo_now } : { at: account.demo_now });
    return get(db, 'SELECT * FROM orders WHERE ref = ?', order.ref);
  });
}

// the kernel's checks, applied after the fact to orders the framework agent booked with no gate in front of it
function violations(order, exposure, dayOrders, L = preset.limits) {
  const out = [];
  if (dayOrders > L.max_orders_per_turn) out.push('orders_per_turn');
  if (exposure.premium + order.premium_cents > L.max_premium_cents) out.push('max_premium');
  if (exposure.hours + order.gpu_hours > L.max_gpu_hours) out.push('max_gpu_hours');
  if (Math.abs(exposure.delta + order.delta_bps) > L.max_abs_delta_bps) out.push('max_delta');
  if (order.kind !== 'lock' && Math.abs(order.strike_cents - order.index_cents) * 10000 / order.index_cents > L.strike_band_bps) out.push('strike_band');
  return out;
}

const limitsText = L => Object.entries(L).map(([k, v]) => `${k} ${v}`).join('\n');
const systemText = [
  `you are ${preset.name}, an autonomous agent trading options on gpu compute on dodona.`,
  `goal: ${preset.goal}`,
  `mandate: ${preset.mandate.gpu} in ${preset.mandate.region}, windows of ${preset.mandate.windowDays} days starting ${preset.mandate.leadDays} days ahead. strikes are integer cents per gpu-hour, premiums are integer cents in total, strike_cents 0 means at the money.`,
  'each day you receive the market. use get_quote to price an idea, place_order to book it, get_exposure to check what you already hold. place at most two orders per day, then answer with one line of reasoning.',
  '\n## hard limits\n' + limitsText(preset.limits),
].join('\n');

/* arm: the kernel, through the production runtime */
async function runKernel() {
  const p = platform();
  const kernel = createKernelClient();
  const muse = createMuseClient({ model: MODEL });
  const runtime = createAgentRuntime({ kernel, muse, boot: async () => p.db, prices: async () => p.history, now: () => p.nowMs });
  const rows = [];
  await runtime.spawn('hedger');
  for (let day = 1; day <= DAYS; day++) {
    if (day > 1) await runtime.tick({ days: 1 });
    const started = performance.now();
    const [t] = await runtime.run({ maxTurns: 1, concurrency: 1 });
    const ms = Math.round(performance.now() - started);
    const u = t.usage ?? {};
    const gc = t.gc?.usage ?? {};
    const booked = t.orders.filter(o => o.ref);
    rows.push({
      day, prompt: u.prompt_tokens ?? 0, maxPrompt: u.prompt_tokens ?? 0, cached: u.cached_tokens ?? 0, completion: u.completion_tokens ?? 0, calls: 1 + (t.gc ? 1 : 0), gcPrompt: gc.prompt_tokens ?? 0,
      microcents: (u.microcents ?? 0) + (gc.microcents ?? 0), ms, modelMs: t.modelMs, contextTokens: t.contextTokens, dropped: t.dropped,
      orders: booked.length, rejected: t.rejected.map(r => r.reason), unsafe: 0, premium: booked.reduce((s, o) => s + cents(o.premiumTotal), 0), note: t.note, syscalls: t.syscalls,
    });
    process.stdout.write(`kernel day ${day}: ctx ${t.contextTokens} prompt ${u.prompt_tokens} orders ${booked.length} rejected ${t.rejected.length} ${t.modelMs}ms ${JSON.stringify(t.syscalls.map(s => ({ kind: s.kind, asked: s.asked_gpu_hours, hours: s.gpu_hours, premium: s.premium_cents, limit: s.limit_premium_cents })))} rejected ${JSON.stringify(t.rejected)}\n`);
  }
  const { procs } = await runtime.ps();
  await kernel.close();
  return { arm: 'kernel', rows, final: { premium: procs[0].premium_spent_cents, hours: procs[0].gpu_hours, orders: procs[0].orders } };
}

/* arm: a LangGraph ReAct agent with a checkpointed thread, the shape of every langgraph trading bot; windowed = same agent, model sees only the last N messages */
async function runFramework(name, { window } = {}) {
  const p = platform();
  const exposure = { premium: 0, hours: 0, delta: 0, orders: 0 };
  let dayOrders = 0, dayUnsafe = [], dayPremium = 0, dayBooked = 0;
  const llm = new ChatOpenAI({ model: MODEL, apiKey: process.env.MODEL_API_KEY, configuration: { baseURL: MUSE_BASE_URL }, maxTokens: 1200, modelKwargs: { reasoning_effort: 'minimal' } });
  const orderInput = z.object({ kind: z.enum(['cap', 'floor', 'lock']), strike_cents: z.number().int().describe('0 means at the money'), gpu_hours: z.number().int().positive() });
  const tools = [
    tool(async input => { const q = price(p.db, p.account(), p.history, input); const { custom, ...rest } = q; return JSON.stringify(rest); }, { name: 'get_quote', description: 'price an option on gpu compute without booking it', schema: orderInput }),
    tool(async input => {
      const account = p.account();
      const q = price(p.db, account, p.history, input);
      const bad = violations(q, exposure, dayOrders + 1);
      const row = book(p.db, account, q, p.history);
      dayOrders += 1; dayBooked += 1; dayPremium += q.premium_cents;
      if (bad.length) dayUnsafe.push(bad.join('+'));
      exposure.premium += q.premium_cents; exposure.hours += q.gpu_hours; exposure.delta += q.delta_bps; exposure.orders += 1;
      return JSON.stringify({ ref: row.ref, kind: q.kind, count: q.count, strike_cents: q.strike_cents, gpu_hours: q.gpu_hours, premium_cents: q.premium_cents, status: row.status });
    }, { name: 'place_order', description: 'book and pay for an option on gpu compute', schema: orderInput }),
    tool(async () => JSON.stringify({ premium_spent_cents: exposure.premium, gpu_hours: exposure.hours, delta_bps: exposure.delta, orders: exposure.orders }), { name: 'get_exposure', description: 'what you already hold: premium spent, gpu-hours covered, delta, order count', schema: z.object({}) }),
  ];
  const preModelHook = window
    ? async state => ({ llmInputMessages: await trimMessages(state.messages, { maxTokens: window, tokenCounter: msgs => msgs.length, strategy: 'last', startOn: 'human', includeSystem: true }) })
    : undefined;
  const agent = createReactAgent({ llm, tools, prompt: systemText, checkpointer: new MemorySaver(), preModelHook });
  const config = { configurable: { thread_id: name } };
  const rows = [];
  let seen = 0;
  for (let day = 1; day <= DAYS; day++) {
    const account = p.account();
    let fired = [];
    if (day > 1) { const r = advance(p.db, account, { days: 1 }, { history: p.history, nowMs: p.nowMs }); fired = r.fired.slice(-5).map(e => `${e.at.slice(0, 10)} ${e.type} ${e.orderRef}`); }
    const snap = snapshot(p.db, p.account(), p.history);
    dayOrders = 0; dayUnsafe = []; dayPremium = 0; dayBooked = 0;
    const text = `day ${day} market ${JSON.stringify(snap)}${fired.length ? '\nevents since yesterday:\n' + fired.join('\n') : ''}\ndecide.`;
    const started = performance.now();
    let result, error = null;
    try { result = await agent.invoke({ messages: [new HumanMessage(text)] }, config); }
    catch (e) { error = e.message; result = { messages: (await agent.getState(config)).values.messages ?? [] }; }
    const ms = Math.round(performance.now() - started);
    const fresh = result.messages.slice(seen); seen = result.messages.length;
    const ai = fresh.filter(m => (m.getType?.() ?? m._getType?.()) === 'ai');
    const usage = ai.map(m => m.usage_metadata ?? {});
    const prompt = usage.reduce((s, u) => s + (u.input_tokens ?? 0), 0);
    const maxPrompt = usage.reduce((s, u) => Math.max(s, u.input_tokens ?? 0), 0);
    const cached = usage.reduce((s, u) => s + (u.input_token_details?.cache_read ?? 0), 0);
    const completion = usage.reduce((s, u) => s + (u.output_tokens ?? 0), 0);
    const last = ai[ai.length - 1];
    const note = typeof last?.content === 'string' ? last.content.slice(0, 120).replace(/\s+/g, ' ') : '';
    rows.push({ day, prompt, maxPrompt, cached, completion, calls: ai.length, microcents: costMicrocents(MODEL, { prompt_tokens: prompt, cached_tokens: cached, completion_tokens: completion }), ms, messages: result.messages.length, orders: dayBooked, unsafe: dayUnsafe.length, unsafeWhy: dayUnsafe, premium: dayPremium, note, error });
    process.stdout.write(`${name} day ${day}: msgs ${result.messages.length} prompt ${prompt} (max ${maxPrompt}) calls ${ai.length} orders ${dayBooked} unsafe ${dayUnsafe.length} ${ms}ms${error ? ' ERROR ' + error.slice(0, 80) : ''}\n`);
  }
  return { arm: name, rows, final: { ...exposure } };
}

const results = [];
for (const arm of ARMS) {
  const started = Date.now();
  const r = arm === 'kernel' ? await runKernel() : await runFramework(arm, arm === 'window' ? { window: WINDOW } : {});
  r.seconds = Math.round((Date.now() - started) / 1000);
  results.push(r);
}
const out = `results-${new Date().toISOString().replace(/[:.]/g, '-')}.json`;
writeFileSync(out, JSON.stringify({ model: MODEL, days: DAYS, window: WINDOW, limits: preset.limits, results }, null, 2));

const sum = (rows, k) => rows.reduce((s, r) => s + (r[k] ?? 0), 0);
console.log(`\n| arm | days | model calls | prompt tokens total | largest prompt day 1 | largest prompt day ${DAYS} | cached | completion | cost ¢ | wall s | orders | unsafe orders | kernel rejections | premium spent $ | gpu-h |`);
console.log('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|');
for (const r of results) {
  const first = r.rows[0], lastRow = r.rows[r.rows.length - 1];
  console.log(`| ${r.arm} | ${r.rows.length} | ${sum(r.rows, 'calls')} | ${sum(r.rows, 'prompt')} | ${first.maxPrompt} | ${lastRow.maxPrompt} | ${sum(r.rows, 'cached')} | ${sum(r.rows, 'completion')} | ${(sum(r.rows, 'microcents') / 1e6).toFixed(2)} | ${r.seconds} | ${sum(r.rows, 'orders')} | ${sum(r.rows, 'unsafe')} | ${r.rows.reduce((s, x) => s + (x.rejected?.length ?? 0), 0)} | ${(r.final.premium / 100).toFixed(2)} | ${r.final.hours} |`);
}
console.log(`\nwrote ${out}`);
