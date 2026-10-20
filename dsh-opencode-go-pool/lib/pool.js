/**
 * A round-robin pool of OpenCode Go API keys.
 *
 * The pool owns rotation, per-key cooldown, and the error classification that
 * decides *whether* rotation is the right answer. It performs no I/O and knows
 * nothing about Cordis or pi-ai, so the rotation policy is testable on its own
 * and the adapter layer stays a thin translation.
 *
 * Keys never enter the pool: it holds credential *references* and receives the
 * already-resolved secrets for one request, which keeps key material out of
 * every long-lived object and every log line.
 *
 * @module dsh-opencode-go-pool/pool
 */

const DEFAULT_BASE_COOLDOWN_MS = 60_000
const DEFAULT_MAX_COOLDOWN_MS = 30 * 60_000

/**
 * Whether a failure means "this account is the problem, try another".
 *
 * Deliberately narrow, because the two wrong answers cost differently. A
 * 401/402/403 is per-account state (expired, out of balance, revoked) and a 429
 * is a rate limit; rotating genuinely fixes both. A 5xx or a network error is
 * *provider-wide*: rotating would spend every account to reach the same
 * failure, so it is rethrown on the first attempt and left to the agent's own
 * retry layer, which retries the turn rather than the key.
 * @param {number | undefined} status - HTTP status, when the failure reached the wire.
 * @param {string} message - failure text, inspected for the provider's own wording.
 * @returns {boolean} whether the pool should cool this key and retry elsewhere.
 */
export function shouldRotate(status, message) {
  if (status === 401 || status === 402 || status === 403 || status === 429) return true
  // Some gateways answer 200 with an error body and some answer 400 for a quota
  // state rather than a malformed request; both carry the wording, not a code.
  // The alternation must match what the provider actually says: OpenCode Go
  // reports an unusable account as `api_key_expired` and as plain prose
  // ("This API key has expired"), so both spellings are listed.
  return /api[\s_-]?key[\s_-]+(?:has\s+)?(?:expired|invalid|revoked)|insufficient|quota|usage limit|rate[\s-]?limit|too many requests/i
    .test(message)
}

/**
 * Extract an HTTP status from an error, however many layers wrapped it.
 * @param {unknown} error - the failure to inspect.
 * @returns {number | undefined} the status, or undefined when none is present.
 */
export function statusOf(error) {
  let current = error
  for (let depth = 0; depth < 8 && current !== null && current !== undefined; depth += 1) {
    if (typeof current !== 'object') break
    for (const value of [current.status, current.statusCode, current.code, current.response?.status]) {
      if (typeof value === 'number' && value >= 100 && value <= 599) return value
    }
    current = current.cause ?? current.error
  }
  return undefined
}

/**
 * Flatten an error chain into one searchable string.
 * @param {unknown} error - the failure to inspect.
 * @returns {string} every message in the chain, joined.
 */
export function messageOf(error) {
  const parts = []
  let current = error
  for (let depth = 0; depth < 8 && current !== null && current !== undefined; depth += 1) {
    if (typeof current === 'string') {
      parts.push(current)
      break
    }
    if (typeof current !== 'object') break
    if (typeof current.message === 'string') parts.push(current.message)
    current = current.cause ?? current.error
  }
  return parts.join(' | ')
}

/**
 * A stable, ordered key set with per-key health.
 *
 * Rotation is strict round-robin from the last key handed out, so N keys serve
 * requests in a predictable cycle. A key that fails is cooled down and skipped.
 * When every key is cooling the pool refuses and reports the earliest expiry,
 * rather than silently retrying a key it just failed.
 */
export class KeyPool {
  /** @type {Array<{ ref: string, cooldownUntil: number, failures: number, uses: number }>} */
  #states
  #baseCooldownMs
  #maxCooldownMs
  #now
  /** Index handed out most recently, so the next pick is its successor. */
  #cursor = -1

  /**
   * @param {readonly string[]} refs - credential references in rotation order; duplicates collapse.
   * @param {{ baseCooldownMs?: number, maxCooldownMs?: number, now?: () => number }} [options] - tuning and clock.
   */
  constructor(refs, options = {}) {
    const seen = new Set()
    this.#states = []
    for (const ref of refs) {
      if (seen.has(ref)) continue
      seen.add(ref)
      this.#states.push({ ref, cooldownUntil: 0, failures: 0, uses: 0 })
    }
    this.#baseCooldownMs = options.baseCooldownMs ?? DEFAULT_BASE_COOLDOWN_MS
    this.#maxCooldownMs = options.maxCooldownMs ?? DEFAULT_MAX_COOLDOWN_MS
    this.#now = options.now ?? Date.now
  }

  /** How many references the pool holds, cooling ones included. */
  get size() {
    return this.#states.length
  }

  /** The pool's references in rotation order. */
  get refs() {
    return this.#states.map(state => state.ref)
  }

  /**
   * Inherit health for references this pool holds from an earlier snapshot.
   *
   * A credential change rebuilds the pool, but a key that was cooling down a
   * second earlier is still cooling down: without this, re-reading the store
   * would hand a just-rate-limited account the very next request.
   * @param {Map<string, { cooldownUntil: number, failures: number, uses: number }>} previous - health keyed by reference.
   */
  adopt(previous) {
    for (const state of this.#states) {
      const inherited = previous.get(state.ref)
      if (inherited === undefined) continue
      state.cooldownUntil = inherited.cooldownUntil
      state.failures = inherited.failures
      state.uses = inherited.uses
    }
  }

  /**
   * Pick the next usable key.
   *
   * Synchronous by contract: `values` is the per-request set of secrets the
   * caller already resolved, so this stays a decision and never hides a network
   * or disk read behind a rotation step.
   * @param {ReadonlyMap<string, string>} values - resolved secrets, by reference.
   * @param {ReadonlySet<string>} [exclude] - references this request already tried.
   * @returns {{ ok: true, ref: string, key: string } | { ok: false, reason: 'empty' | 'all-cooling', retryAfterMs: number }} the verdict.
   */
  acquire(values, exclude = new Set()) {
    if (this.#states.length === 0) return { ok: false, reason: 'empty', retryAfterMs: 0 }
    const now = this.#now()
    let earliest = Number.POSITIVE_INFINITY
    for (let step = 1; step <= this.#states.length; step += 1) {
      const index = (this.#cursor + step) % this.#states.length
      const state = this.#states[index]
      if (state.cooldownUntil > now) {
        earliest = Math.min(earliest, state.cooldownUntil)
        continue
      }
      if (exclude.has(state.ref)) continue
      const key = values.get(state.ref)
      // A reference that does not resolve is unusable, not a failed account:
      // the operator removed or mistyped it, and no cooldown is warranted.
      if (key === undefined || key.length === 0) continue
      this.#cursor = index
      state.uses += 1
      return { ok: true, ref: state.ref, key }
    }
    return {
      ok: false,
      reason: 'all-cooling',
      retryAfterMs: earliest === Number.POSITIVE_INFINITY ? 0 : Math.max(0, earliest - now),
    }
  }

  /**
   * Record that a key served a request, clearing its cooldown.
   * @param {string} ref - the key's credential reference.
   */
  succeed(ref) {
    const state = this.#states.find(entry => entry.ref === ref)
    if (state === undefined) return
    state.cooldownUntil = 0
    state.failures = 0
  }

  /**
   * Cool a key down after a failure that rotation can fix.
   * @param {string} ref - the key's credential reference.
   * @returns {number} the cooldown applied, in milliseconds, for the log line.
   */
  fail(ref) {
    const state = this.#states.find(entry => entry.ref === ref)
    if (state === undefined) return 0
    state.failures += 1
    const cooldown = Math.min(this.#maxCooldownMs, this.#baseCooldownMs * 2 ** (state.failures - 1))
    state.cooldownUntil = this.#now() + cooldown
    return cooldown
  }

  /**
   * A redacted view for logs and diagnostics: references, health, counts — never
   * key material, because this is the shape that reaches a log sink.
   * @returns {Array<{ ref: string, cooling: boolean, cooldownMs: number, uses: number, failures: number }>} one row per reference.
   */
  snapshot() {
    const now = this.#now()
    return this.#states.map(state => ({
      ref: state.ref,
      cooling: state.cooldownUntil > now,
      cooldownMs: Math.max(0, state.cooldownUntil - now),
      uses: state.uses,
      failures: state.failures,
    }))
  }

  /**
   * Health rows keyed by reference, for {@link adopt} after a rebuild.
   * @returns {Map<string, { cooldownUntil: number, failures: number, uses: number }>} one row per reference.
   */
  health() {
    return new Map(this.#states.map(state => [
      state.ref,
      { cooldownUntil: state.cooldownUntil, failures: state.failures, uses: state.uses },
    ]))
  }
}
