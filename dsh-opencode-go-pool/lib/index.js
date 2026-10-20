/**
 * OpenCode Go, served through a round-robin account pool.
 *
 * ## Why this exists
 *
 * `@deepseek-ai/dsh-llm-pi-ai` already ships an `opencode-go` catalog provider,
 * so *using* one OpenCode Go key is pure configuration. What it cannot express
 * is a second key: `apiKeyEnv` names exactly one credential reference, and a key
 * that reaches its usage limit reports that limit to the user. This plugin adds
 * the missing piece — a pool — while delegating every protocol decision back to
 * the shipped catalog.
 *
 * ## Where the pool reaches the wire
 *
 * The route registers a `PiAiAdapter` whose options resolve a *different*
 * credential per request: the base class calls `resolveApiKey()` once per
 * stream call and freezes the answer for that call, so answering it from the
 * pool changes the account and nothing else. Model descriptors, the per-model
 * wire format (`anthropic-messages`, `openai-completions` and
 * `openai-responses` all coexist on this one route), reasoning levels, request
 * headers, the idle watchdog and attachment budgets stay the base class's own
 * behaviour.
 *
 * ## Where rotation lives
 *
 * `stream()` and `prepareCall()` are the adapter's public entry points, and the
 * pool drives them: acquire a key, run the base class's body, and on a failure
 * another account could fix, cool that key down and run it again on the next
 * one. `super.stream()` rebuilds the whole request per call, so a retry re-sends
 * the request rather than resuming a half-consumed stream.
 *
 * ## Secrets
 *
 * Keys are read through the harness credential seam per request and are never
 * copied into configuration, into the pool, or into a log line. The pool holds
 * references only, and `KeyPool.snapshot()` is the shape that reaches a log.
 *
 * @module dsh-opencode-go-pool
 */

import { credentialRef } from '@deepseek-ai/dsh-credentials'
import { resolveRetryPolicy } from '@deepseek-ai/dsh-llm'
import { PiAiAdapter } from '@deepseek-ai/dsh-llm-pi-ai'
import { builtinProviders } from '@earendil-works/pi-ai/providers/all'
import z from '@deepseek-ai/schemastery'

import { KeyPool, messageOf, shouldRotate, statusOf } from './pool.js'

/** The pi-ai catalog provider route this plugin serves. */
export const PROVIDER = 'opencode-go'

/** The reference name the first pooled key conventionally lives under. */
const PRIMARY_REF = 'OPENCODE_GO_API_KEY'

/** References discovered when the configuration names none. */
const DISCOVERY_PATTERN = /^OPENCODE_GO_API_KEY(?:_\d+)?$/

/** How many `OPENCODE_GO_API_KEY_<n>` slots discovery probes past the primary one. */
const DISCOVERY_SLOTS = 32

/** Plugin configuration. */
export const Config = z.object({
  /**
   * Credential references that make up the pool, in rotation order. Empty means
   * discover: `OPENCODE_GO_API_KEY` plus every `OPENCODE_GO_API_KEY_<n>` that
   * currently resolves, ordered by that suffix.
   */
  keys: z.array(z.string()).default([]),
  /** Display name for the route in model selectors. */
  displayName: z.string().default('OpenCode Go'),
  /** First cooldown applied to a failing key; doubles on each repeat failure. */
  baseCooldownMs: z.number().step(1).min(1).default(60_000),
  /** Ceiling for that exponential cooldown. */
  maxCooldownMs: z.number().step(1).min(1).default(1_800_000),
  /** How many keys one request may try before the failure reaches the agent. */
  maxAttemptsPerRequest: z.number().step(1).min(1).default(8),
})

/** Numeric suffix of a reference, so `_2` sorts after `_1` and `_10` after `_2`. */
function discoveryOrder(ref) {
  const match = /_(\d+)$/.exec(ref)
  return match === null ? 0 : Number(match[1])
}

/**
 * Resolve the pool's references.
 * @param configured - the configuration's explicit list, in rotation order.
 * @param stored - every reference that currently resolves.
 * @returns references to pool, deduplicated, the explicit list taking precedence.
 */
export function resolveRefs(configured, stored) {
  const explicit = configured.map(ref => ref.trim()).filter(ref => ref.length > 0)
  const discovered = explicit.length > 0
    ? []
    : stored
      .filter(ref => DISCOVERY_PATTERN.test(ref))
      .sort((left, right) => discoveryOrder(left) - discoveryOrder(right) || left.localeCompare(right))
  return [...new Set([...explicit, ...discovered])]
}

/**
 * The candidate references discovery probes when the configuration names none.
 * @returns the conventional pool slot names, primary first.
 */
export function discoveryCandidates() {
  const candidates = [PRIMARY_REF]
  for (let slot = 2; slot <= DISCOVERY_SLOTS; slot += 1) candidates.push(`${PRIMARY_REF}_${slot}`)
  return candidates
}

/**
 * The one credential edge the base adapter requires but this route never uses.
 *
 * The base adapter needs a pi-ai credential store and auth context because a
 * route might authenticate through a login. This route cannot: the pool hands
 * every request its key. The store still has to exist and answer honestly, so
 * it reports "nothing stored" and refuses writes rather than pretending.
 */
const INERT_AUTH = {
  credentials: {
    async read() {
      return undefined
    },
    async list() {
      return []
    },
    async modify() {
      throw new Error('opencode-go-pool: this route authenticates through the pool and stores no pi-ai credential')
    },
    async delete() {},
  },
  authContext: {
    async env() {
      return undefined
    },
    async fileExists() {
      return false
    },
  },
}

/**
 * The pooled adapter: the shipped pi-ai adapter plus a key pool in front of it.
 *
 * It overrides exactly two things — which account a request uses, and what
 * happens when that account fails — and inherits the request body itself, so a
 * base-class improvement reaches this route without a change here.
 */
class PooledPiAiAdapter extends PiAiAdapter {
  /**
   * @param options - the base adapter's own options.
   * @param pool - the pool to draw keys from.
   * @param readValues - resolves every pooled reference to its secret for one request.
   * @param log - the plugin logger.
   * @param maxAttempts - how many keys one request may try.
   */
  constructor(options, pool, readValues, log, maxAttempts) {
    super(options)
    this.pool = pool
    this.readValues = readValues
    this.log = log
    this.maxAttempts = maxAttempts
    /** The key this in-flight call froze, read by the base class's hook. */
    this.currentKey = undefined
  }

  /**
   * The base class's per-request credential hook.
   *
   * It calls this once per stream call and freezes the answer for that call,
   * which is exactly the lifetime a pooled account should have.
   * @returns the key selected for the request in flight.
   */
  resolveApiKey() {
    return Promise.resolve(this.currentKey)
  }

  /**
   * Serve one request, rotating accounts when a failure is account-shaped.
   * @param options - the agent's generate options.
   */
  async * stream(options) {
    yield* this.#rotate(options, attemptOptions => super.stream(attemptOptions))
  }

  /**
   * Prepare one call, wrapping the stream it hands back in the same rotation.
   * @param provider - the route being called.
   * @param model - the model being called.
   * @param signal - the caller's abort signal.
   */
  async prepareCall(provider, model, signal) {
    const prepared = await super.prepareCall(provider, model, signal)
    return {
      ...prepared,
      stream: options => this.#rotate(options, attemptOptions => prepared.stream(attemptOptions)),
    }
  }

  /**
   * Run one request across pooled accounts.
   * @param options - the request.
   * @param run - how to start one attempt on the frozen key.
   */
  async * #rotate(options, run) {
    const pool = this.pool
    const values = await this.readValues()
    const limit = Math.max(1, Math.min(this.maxAttempts, pool.size))
    const tried = new Set()
    let lastError
    for (let attempt = 0; attempt < limit; attempt += 1) {
      const lease = pool.acquire(values, tried)
      if (!lease.ok) {
        if (lastError !== undefined) throw lastError
        const seconds = Math.ceil(lease.retryAfterMs / 1000)
        throw new Error(
          lease.reason === 'empty'
            ? 'opencode-go-pool: the pool holds no OpenCode Go credential; store OPENCODE_GO_API_KEY through the'
              + " harness credentials service (the Models page writes it) or name one in this plugin's keys list"
            : `opencode-go-pool: every pooled OpenCode Go key is cooling down; the earliest frees in ${seconds}s`,
        )
      }
      tried.add(lease.ref)
      this.currentKey = lease.key
      try {
        for await (const chunk of run(options)) yield chunk
        pool.succeed(lease.ref)
        return
      } catch (error) {
        lastError = error
        if (options.signal?.aborted === true) throw error
        const status = statusOf(error)
        if (!shouldRotate(status, messageOf(error))) throw error
        const cooldown = pool.fail(lease.ref)
        this.log.warn(
          `opencode-go-pool: key ${lease.ref} failed (${status ?? 'no status'}); cooling it for`
          + ` ${Math.round(cooldown / 1000)}s and retrying on the next pooled key`,
        )
      } finally {
        this.currentKey = undefined
      }
    }
    throw lastError
  }
}

/**
 * Mount the pooled OpenCode Go route.
 * @param ctx - the plugin context.
 * @param config - the validated plugin configuration.
 */
export function apply(ctx, config) {
  const log = ctx.logger

  /**
   * Resolve one reference to its current value.
   *
   * Per call, never cached: a key replaced through the Models page or an
   * externally edited environment must reach the next request, and a cached
   * secret would survive exactly the change the operator wanted applied.
   * @param ref - the reference to resolve.
   * @returns the value, or undefined while unconfigured.
   */
  const resolveOne = async (ref) => {
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(ref)) return undefined
    const credentials = ctx.get('credentials')
    if (credentials === undefined) return undefined
    try {
      const hit = await credentials.resolve(credentialRef(ref))
      return typeof hit?.value === 'string' && hit.value.length > 0 ? hit.value : undefined
    } catch {
      // A malformed or unreadable reference is "not configured" here; the
      // caller distinguishes an empty pool from a failing one by its size.
      return undefined
    }
  }

  let pool = new KeyPool([], { baseCooldownMs: config.baseCooldownMs, maxCooldownMs: config.maxCooldownMs })
  let adapter
  let registration

  /** Resolve every pooled reference at once, for one request. */
  const readValues = async () => {
    const entries = await Promise.all(pool.refs.map(async ref => [ref, await resolveOne(ref)]))
    return new Map(entries.filter(([, value]) => typeof value === 'string' && value.length > 0))
  }

  /**
   * Rebuild the pool, then reconcile the route with what it now holds.
   *
   * A plugin with no OpenCode Go credential stays dormant and registers
   * nothing — the posture the bundled adapter takes for an empty `providers`
   * dict — so a profile without the subscription never sees the provider in a
   * model picker.
   */
  const refresh = async () => {
    const candidates = resolveRefs(config.keys, config.keys.length > 0 ? config.keys : discoveryCandidates())
    const usable = []
    for (const ref of candidates) {
      if (await resolveOne(ref) !== undefined) usable.push(ref)
    }
    const rebuilt = new KeyPool(
      usable,
      { baseCooldownMs: config.baseCooldownMs, maxCooldownMs: config.maxCooldownMs },
    )
    // A key cooling down a second ago is still cooling down: re-reading the
    // credential plane must not hand a just-rate-limited account the next call.
    rebuilt.adopt(pool.health())
    pool = rebuilt
    if (adapter !== undefined) adapter.pool = pool
    if (usable.length === 0) {
      log.debug('opencode-go-pool: no OpenCode Go credential resolved; the route stays dormant')
      return
    }
    if (registration === undefined) {
      const catalog = builtinProviders().find(entry => entry.id === PROVIDER)
      if (catalog === undefined) {
        log.error(`opencode-go-pool: the installed pi-ai catalog has no "${PROVIDER}" provider; nothing registered`)
        return
      }
      const models = catalog.getModels()
      const profile = {
        provider: PROVIDER,
        displayName: config.displayName,
        streamIdleTimeoutMs: 300_000,
        retryPolicy: resolveRetryPolicy(undefined, 'opencode-go-pool'),
        configuredMaxTokens: new Map(),
        maxRequestImageBytes: 20 * 1024 * 1024,
        requestImagePixelBudget: 2048 * 2048,
        requestImageMaxBytes: 1024 * 1024,
        // The catalog provider keeps each model's own protocol and endpoint;
        // only the display identity is this plugin's.
        piProvider: { ...catalog, id: PROVIDER, name: config.displayName, getModels: () => models },
      }
      const profiles = new Map([[PROVIDER, profile]])
      adapter = new PooledPiAiAdapter({
        profiles: () => profiles,
        // Answered by the pool through the adapter's own frozen key; the base
        // adapter's per-request credential resolution is never consulted.
        resolveApiKey: () => Promise.resolve(adapter.currentKey),
        auth: INERT_AUTH,
        resolveAttachments: () => ctx.get('attachments'),
      }, pool, readValues, log, config.maxAttemptsPerRequest)
      registration = ctx.llm.registerAdapter([PROVIDER], adapter)
      log.info(`opencode-go-pool: route "${PROVIDER}" serving ${models.length} models over ${usable.length} key(s)`)
    } else {
      registration.replace([PROVIDER])
    }
  }

  void refresh()
  // A key added, replaced, or removed through the Models page reaches the next
  // request without a restart.
  ctx.inject(['credentials'], () => {
    void refresh()
  })
}
