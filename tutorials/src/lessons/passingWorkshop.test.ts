import { describe, expect, it } from 'vitest'
import { simulate, enumerateOrderings, replay, readLocation, href, guided, type Operation, type Event } from './passingWorkshop'

const full: Operation[] = ['push', 'push']
describe('the pinned queue contract', () => {
  it('releases the mutex while full, wakes on pop, then reuses the freed slot', () => {
    const waiting = simulate([...full, 'push'])
    expect(waiting.producer).toMatchObject({ phase: 'waiting', item: { id: 3 } })
    expect(waiting.mutexOwner).toBeNull()
    const woken = simulate([...full, 'push', 'pop'])
    expect(woken.producer.phase).toBe('notified')
    expect(woken.slots.map(j => j?.id ?? null)).toEqual([null, 2])
    const resumed = simulate([...full, 'push', 'pop', 'resume-P'])
    expect(resumed.slots.map(j => j?.id)).toEqual([3, 2])
    expect([resumed.head, resumed.tail, resumed.size]).toEqual([1, 1, 2])
    expect(resumed.accepted).toEqual([1, 2, 3])
  })
  it('does not equate notification with a completed pop', () => {
    expect(simulate(['pop']).consumer.phase).toBe('waiting')
    expect(() => simulate(['pop', 'resume-C'])).toThrow()
    expect(simulate(['pop', 'push']).delivered).toEqual([])
    expect(simulate(['pop', 'push', 'resume-C']).delivered).toEqual([1])
  })
  it('rejects full try_push and consumes a movable rvalue before testing capacity', () => {
    const failed = simulate([...full, 'try-rvalue'])
    expect(failed.candidate).toBe('moved-from')
    expect(failed.dropped).toEqual([3])
    const retry = simulate([...full, 'try-rvalue', 'pop', 'try-rvalue'])
    expect(retry.slots[0]).toMatchObject({ id: 4, payload: 'moved-from' })
    expect(simulate([...full, 'try-lvalue']).candidate).toBe('job')
  })
  it('close is idempotent; pending producer rejects, items drain, failed pop preserves out', () => {
    const s = simulate([...full, 'push', 'close', 'close', 'pop', 'resume-P', 'pop', 'pop'])
    expect(s.delivered).toEqual([1, 2])
    expect(s.dropped).toEqual([3])
    expect(s.size).toBe(0)
    expect(s.closed).toBe(true)
    expect(s.out?.id).toBe(2)
    expect(s.log.at(-1)?.result).toBe('false')
  })
  it('wakes an empty consumer on close but allows destruction to overtake resumption', () => {
    const s = simulate(['pop', 'close', 'destroy'])
    expect(s.ub).toBeTruthy()
    expect(s.delivered).toEqual([])
    expect(() => simulate(['pop', 'close', 'destroy', 'resume-C'])).toThrow(/undefined/)
    expect(simulate(['pop', 'close', 'resume-C', 'finish-P', 'finish-C', 'join', 'destroy']).ub).toBeNull()
    expect(simulate(['destroy']).ub).toBeTruthy()
  })
  it('accounts for undrained items destroyed after users finish', () => {
    const s = simulate([...full, 'finish-P', 'finish-C', 'join', 'destroy'])
    expect(s.destroyedItems).toEqual([1, 2])
    expect(s.slots).toEqual([null, null])
  })
  it('rejects concurrent calls on the same blocked actor and use after finishing', () => {
    expect(() => simulate([...full, 'push', 'try-push'])).toThrow()
    expect(() => simulate(['finish-C', 'pop'])).toThrow()
    expect(() => simulate(['pop', 'finish-C'])).toThrow()
  })
})

describe('exhaustive small operation spaces', () => {
  for (const policy of ['blocking', 'trying'] as const) {
    it(`enumerates all third push/pop orders for ${policy}`, () => {
      const rows = enumerateOrderings(policy, 'full')
      expect(rows).toHaveLength(2)
      expect(rows.map(r => r.state.accepted.includes(3))).toEqual(policy === 'blocking' ? [true, true] : [false, true])
    })
  }
  it('includes close both before and after pending calls resume', () => {
    expect(enumerateOrderings('blocking', 'pending-push')).toHaveLength(4)
    expect(enumerateOrderings('blocking', 'pending-pop')).toHaveLength(4)
  })
  it('conserves identities at every prefix of every small schedule', () => {
    let checked = 0
    function explore(trace: Operation[], depth: number) {
      const s = simulate(trace)
      const occupied = s.slots.flatMap(j => j ? [j.id] : [])
      expect(occupied.length).toBe(s.size)
      expect(s.size).toBeLessThanOrEqual(2)
      const accounted = [...occupied, ...s.delivered, ...s.destroyedItems]
      expect([...accounted].sort((a, b) => a - b)).toEqual([...s.accepted].sort((a, b) => a - b))
      expect(new Set(accounted).size).toBe(accounted.length)
      const attempted = [...s.accepted, ...s.dropped, ...(s.producer.item ? [s.producer.item.id] : [])]
      expect([...attempted].sort((a, b) => a - b)).toEqual(Array.from({ length: s.nextId - 1 }, (_, i) => i + 1))
      checked++
      if (!depth || s.ub || s.destroyed) return
      for (const op of ['push', 'try-push', 'pop', 'try-pop', 'close', 'resume-P', 'resume-C'] as Operation[]) {
        try {
          simulate([...trace, op])
        } catch {
          continue
        }
        explore([...trace, op], depth - 1)
      }
    }
    explore([], 5)
    expect(checked).toBeGreaterThan(2000)
  })
})

describe('checkpoint and URL history', () => {
  it('checks all six tasks and restores every decision', () => {
    const events: Event[] = []
    for (let task = 0; task < 6; task++) {
      events.push(...guided[task], 'verify')
      expect(replay(events).verdict?.ok).toBe(true)
      if (task < 5) events.push('next')
    }
    expect(readLocation(`https://example.test/${href(events)}`).events).toEqual(events)
    expect(replay(events).completed).toEqual([0, 1, 2, 3, 4])
  })
  it('accepts every legal drained trace, including rejection after the final pop', () => {
    const prefix: Event[] = [...guided[0], 'verify', 'next', ...guided[1], 'verify', 'next', ...guided[2], 'verify', 'next']
    for (const trace of [['close', 'push', 'pop', 'pop', 'pop'], ['pop', 'close', 'pop', 'pop', 'push'], ['pop', 'pop', 'close', 'push', 'pop']] as Operation[][]) {
      expect(replay([...prefix, ...trace, 'verify']).verdict?.ok).toBe(true)
    }
  })
  it('does not count a merely full state as demonstrating wait and resumption', () => {
    expect(replay(['push', 'push', 'verify']).verdict?.ok).toBe(false)
  })
  it('rejects invalid, out-of-order, old, overlong and post-check events', () => {
    for (const events of [['next'], ['ownership-husk'], ['wat'], [...guided[0], 'verify', 'push'], Array(129).fill('push')]) {
      expect(() => replay(events as Event[])).toThrow()
    }
    expect(readLocation('https://example.test/?lesson=passing-an-item&v=0').error).toBeTruthy()
    expect(readLocation('https://example.test/?lesson=passing-an-item&v=1&events=wat').events).toEqual([])
  })
})

it('does not credit a different waiting item or a full rejection as the requested witness', () => {
  expect(replay(['push', 'push', 'pop', 'push', 'push', 'pop', 'resume-P', 'verify']).verdict?.ok).toBe(false)
  const second: Event[] = [...guided[0], 'verify', 'next']
  expect(replay([...second, 'push', 'pop', 'pop', 'push', 'resume-C', 'verify']).verdict?.ok).toBe(false)
  const drain: Event[] = [...second, ...guided[1], 'verify', 'next', ...guided[2], 'verify', 'next']
  expect(replay([...drain, 'try-push', 'pop', 'pop', 'close', 'pop', 'verify']).verdict?.ok).toBe(false)
})

it('enumerates every legal close/resumption order and its actual outcome', () => {
  const pushes = enumerateOrderings('blocking', 'pending-push')
  expect(pushes.map(r => r.trace.slice(3).join(','))).toEqual(['pop,close,resume-P', 'pop,resume-P,close', 'close,pop,resume-P', 'close,resume-P,pop'])
  expect(pushes.map(r => r.state.accepted.includes(3))).toEqual([false, true, false, false])
  const pops = enumerateOrderings('blocking', 'pending-pop')
  expect(pops.map(r => r.trace.slice(1).join(','))).toEqual(['push,close,resume-C', 'push,resume-C,close', 'close,push,resume-C', 'close,resume-C,push'])
  expect(pops.map(r => r.state.delivered)).toEqual([[1], [1], [], []])
})
