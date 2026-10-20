/**
 * End-to-end check: does the pooled adapter actually rotate on an account
 * failure, and does it leave provider-wide failures un-rotated?
 *
 * This runs the real `PooledPiAiAdapter` against a real local HTTP server that
 * speaks the OpenAI-completions SSE dialect, with the real pi-ai catalog behind
 * it — only `baseURL` is repointed at the server. The assertions are about
 * observable wire traffic (which Authorization header arrived, how many
 * requests), so a rotation that silently did nothing cannot pass.
 *
 * Run: node tests/e2e.mjs
 */

import assert from 'node:assert/strict'
import { createServer } from 'node:http'

const HERE = new URL('../lib/index.js', import.meta.url)

/** Requests the mock server saw: `{ authorization, model }` per call. */
const seen = []

/** Per-key behaviour, set by each scenario. */
let behaviour = () => ({ status: 200 })

/** A minimal OpenAI-completions SSE body carrying one assistant message. */
function sseBody() {
  return [
    'data: {"id":"x","object":"chat.completion.chunk","created":1,"model":"m","choices":[{"index":0,"delta":{"role":"assistant","content":"ok"},"finish_reason":null}]}',
    'data: {"id":"x","object":"chat.completion.chunk","created":1,"model":"m","choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":1,"completion_tokens":1,"total_tokens":2}}',
    'data: [DONE]',
    '',
  ].join('\n\n')
}

const server = createServer((request, response) => {
  const authorization = request.headers.authorization ?? ''
  seen.push({ authorization, url: request.url })
  const verdict = behaviour(authorization)
  if (verdict.status !== 200) {
    const body = JSON.stringify({ error: { type: 'rate_limit', message: verdict.message ?? 'limited' } })
    response.writeHead(verdict.status, { 'content-type': 'application/json', 'content-length': Buffer.byteLength(body) })
    response.end(body)
    return
  }
  const body = sseBody()
  response.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-cache' })
  response.end(body)
})

await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
const { port } = server.address()
const baseURL = `http://127.0.0.1:${port}/v1`

// The plugin's module graph expects the harness packages to resolve, which they
// only do inside a dsh profile. Rather than fake them, this test imports the
// adapter through the same resolution the profile uses.
const { apply, resolveRefs, PROVIDER } = await import(HERE.href)

console.log('module loaded:', typeof apply === 'function', '| provider:', PROVIDER)

/** The keys the fake credential seam holds, by reference. */
const secrets = new Map([
  ['OPENCODE_GO_API_KEY', 'key-one'],
  ['OPENCODE_GO_API_KEY_2', 'key-two'],
])

/** A logger that records instead of printing, so the test can assert on it. */
const logs = []
const logger = {
  debug: message => logs.push(['debug', message]),
  info: message => logs.push(['info', message]),
  warn: message => logs.push(['warn', message]),
  error: message => logs.push(['error', message]),
}

/** The pieces of a plugin context this plugin actually touches. */
function makeContext() {
  let route
  const credentials = {
    async resolve(ref) {
      const value = secrets.get(ref.name)
      return value === undefined ? undefined : { value, source: 'test' }
    },
  }
  const ctx = {
    logger,
    get: name => (name === 'credentials' ? credentials : name === 'attachments' ? undefined : undefined),
    inject: (_deps, run) => run(ctx),
    llm: {
      registerAdapter(routes, adapter) {
        route = { routes, adapter }
        const handle = () => {}
        handle.replace = () => {}
        return handle
      },
    },
  }
  return { ctx, route: () => route }
}

const { ctx, route } = makeContext()
apply(ctx, {
  keys: [],
  displayName: 'OpenCode Go',
  baseCooldownMs: 60_000,
  maxCooldownMs: 1_800_000,
  maxAttemptsPerRequest: 4,
})

// `refresh()` is async; give it a turn to register.
await new Promise(resolve => setTimeout(resolve, 50))

const registered = route()
assert.ok(registered !== undefined, 'the route must register once a key resolves')
assert.deepEqual(registered.routes, [PROVIDER])

// Repoint every model at the mock server: the catalog's real baseURL would
// otherwise reach the internet, and this test is about rotation, not routing.
const adapter = registered.adapter
const profiles = adapter.config.profiles()
const profile = profiles.get(PROVIDER)
assert.ok(profile !== undefined, 'the route must carry a profile')
for (const model of profile.piProvider.getModels()) model.baseUrl = baseURL
for (const model of profile.piProvider.getModels()) {
  // pi-ai reads these back off the model, so a shallow repoint is enough.
  assert.equal(typeof model.api, 'string')
}

/** Collect one request's chunks, returning the text and any thrown error. */
async function runOnce() {
  const options = {
    provider: PROVIDER,
    model: profile.piProvider.getModels().find(model => model.api === 'openai-completions').id,
    messages: [{ role: 'user', content: [{ type: 'text', text: 'hi' }] }],
  }
  const chunks = []
  try {
    for await (const chunk of adapter.stream(options)) chunks.push(chunk)
    return { ok: true, chunks }
  } catch (error) {
    return { ok: false, error }
  }
}

// ---- Scenario 1: the first key is rate-limited, the second serves -----------
seen.length = 0
behaviour = authorization => authorization === 'Bearer key-one'
  ? { status: 429, message: 'rate limit reached' }
  : { status: 200 }

const first = await runOnce()
assert.equal(first.ok, true, `a rate-limited key must not fail the request: ${first.error?.message}`)
assert.equal(seen.length, 2, `expected two wire requests (one per key), saw ${seen.length}`)
assert.equal(seen[0].authorization, 'Bearer key-one')
assert.equal(seen[1].authorization, 'Bearer key-two')
console.log('scenario 1 ok: 429 on key-one rotated to key-two and completed')

// ---- Scenario 2: rotation order continues with the healthy key --------------
seen.length = 0
behaviour = () => ({ status: 200 })
await runOnce()
assert.equal(seen.length, 1)
assert.equal(seen[0].authorization, 'Bearer key-two', 'the cooling key must still be skipped')
console.log('scenario 2 ok: the cooling key is skipped on the next request')

// ---- Scenario 3: a provider-wide failure is not rotated ---------------------
seen.length = 0
behaviour = () => ({ status: 503, message: 'upstream unavailable' })
const third = await runOnce()
assert.equal(third.ok, false, 'a 503 must surface rather than spend every key')
assert.equal(seen.length, 1, `a 503 must not rotate, saw ${seen.length} requests`)
console.log('scenario 3 ok: a 503 failed fast without burning the pool')

server.close()
console.log('\nall rotation checks passed')
