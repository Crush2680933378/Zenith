/**
 * Unit checks for the pool's rotation policy.
 *
 * These cover the behaviour the plugin exists for — round-robin order, cooldown
 * skip and recovery, error classification — without a booted harness, because
 * the pool has no I/O and no Cordis dependency.
 *
 * Run: node --test tests/
 */

import assert from 'node:assert/strict'
import test from 'node:test'

import { KeyPool, messageOf, shouldRotate, statusOf } from '../lib/pool.js'

const valuesOf = (...keys) => new Map(keys.map((key, index) => [`K${index + 1}`, key]))

test('rotation is strict round-robin over healthy keys', () => {
  const pool = new KeyPool(['K1', 'K2', 'K3'])
  const values = valuesOf('a', 'b', 'c')
  const picks = [pool.acquire(values), pool.acquire(values), pool.acquire(values), pool.acquire(values)]
  assert.deepEqual(picks.map(pick => pick.ref), ['K1', 'K2', 'K3', 'K1'])
  assert.deepEqual(picks.map(pick => pick.key), ['a', 'b', 'c', 'a'])
})

test('a failed key is skipped until its cooldown expires', () => {
  let now = 1_000_000
  const pool = new KeyPool(['K1', 'K2'], { baseCooldownMs: 1_000, now: () => now })
  const values = valuesOf('a', 'b')

  assert.equal(pool.acquire(values).ref, 'K1')
  assert.equal(pool.fail('K1'), 1_000)

  // K1 is cooling, so the cycle serves K2 and keeps doing so.
  assert.equal(pool.acquire(values).ref, 'K2')
  assert.equal(pool.acquire(values).ref, 'K2')

  now += 1_000
  assert.equal(pool.acquire(values).ref, 'K1')
})

test('cooldown grows with repeat failures and is capped', () => {
  const pool = new KeyPool(['K1'], { baseCooldownMs: 100, maxCooldownMs: 250, now: () => 0 })
  assert.equal(pool.fail('K1'), 100)
  assert.equal(pool.fail('K1'), 200)
  assert.equal(pool.fail('K1'), 250)
})

test('success clears the failure count', () => {
  const pool = new KeyPool(['K1'], { baseCooldownMs: 100, now: () => 0 })
  pool.fail('K1')
  pool.fail('K1')
  pool.succeed('K1')
  // The count restarted, so the next cooldown is the base one again.
  assert.equal(pool.fail('K1'), 100)
})

test('an exhausted pool reports the earliest recovery instead of retrying', () => {
  let now = 5_000
  const pool = new KeyPool(['K1', 'K2'], { baseCooldownMs: 10_000, now: () => now })
  pool.fail('K1')
  now += 4_000
  pool.fail('K2')

  const lease = pool.acquire(valuesOf('a', 'b'))
  assert.equal(lease.ok, false)
  assert.equal(lease.reason, 'all-cooling')
  // K1 frees first: it started cooling 4s earlier.
  assert.equal(lease.retryAfterMs, 6_000)
})

test('an empty pool is distinguished from an exhausted one', () => {
  const lease = new KeyPool([]).acquire(new Map())
  assert.deepEqual(lease, { ok: false, reason: 'empty', retryAfterMs: 0 })
})

test('exclude advances past a key this request already tried', () => {
  const pool = new KeyPool(['K1', 'K2'])
  const values = valuesOf('a', 'b')
  assert.equal(pool.acquire(values).ref, 'K1')
  assert.equal(pool.acquire(values, new Set(['K1'])).ref, 'K2')
})

test('a reference that does not resolve is skipped without a cooldown', () => {
  const pool = new KeyPool(['K1', 'K2'])
  const lease = pool.acquire(new Map([['K2', 'b']]))
  assert.equal(lease.ok, true)
  assert.equal(lease.ref, 'K2')
  assert.equal(pool.snapshot().find(row => row.ref === 'K1').cooling, false)
})

test('adopt carries health across a rebuild', () => {
  let now = 9_000
  const before = new KeyPool(['K1', 'K2'], { baseCooldownMs: 1_000, now: () => now })
  before.fail('K1')

  const after = new KeyPool(['K1', 'K2'], { baseCooldownMs: 1_000, now: () => now })
  after.adopt(before.health())

  assert.equal(after.snapshot().find(row => row.ref === 'K1').cooling, true)
  // A reference dropped from the new pool is simply absent.
  const shrunk = new KeyPool(['K2'], { now: () => now })
  shrunk.adopt(before.health())
  assert.deepEqual(shrunk.refs, ['K2'])
})

test('snapshot never carries key material', () => {
  const pool = new KeyPool(['K1'])
  pool.acquire(valuesOf('super-secret'))
  const serialized = JSON.stringify(pool.snapshot())
  assert.equal(serialized.includes('super-secret'), false)
})

test('error classification separates account failures from provider failures', () => {
  for (const status of [401, 402, 403, 429]) {
    assert.equal(shouldRotate(status, ''), true, `status ${status} should rotate`)
  }
  for (const status of [500, 502, 503, 400, 404]) {
    assert.equal(shouldRotate(status, ''), false, `status ${status} should not rotate`)
  }
  assert.equal(shouldRotate(undefined, 'This API key has expired.'), true)
  assert.equal(shouldRotate(undefined, 'insufficient credit'), true)
  assert.equal(shouldRotate(undefined, 'Rate limit reached'), true)
  assert.equal(shouldRotate(undefined, 'network socket hang up'), false)
})

test('statusOf reads a status through a wrapped error chain', () => {
  assert.equal(statusOf({ status: 429 }), 429)
  assert.equal(statusOf({ cause: { statusCode: 401 } }), 401)
  assert.equal(statusOf({ response: { status: 503 } }), 503)
  assert.equal(statusOf(new Error('plain')), undefined)
  // A numeric code that is not an HTTP status must not be mistaken for one.
  assert.equal(statusOf({ code: 2 }), undefined)
})

test('messageOf flattens a cause chain', () => {
  const error = new Error('outer', { cause: new Error('inner') })
  const message = messageOf(error)
  assert.equal(message.includes('outer'), true)
  assert.equal(message.includes('inner'), true)
})
