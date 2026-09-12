import { eventsThroughTask, HISTORY_LIMIT, lessonHref, readLessonLocation } from './workshopHistory'
import { choices, correct, taskChoices, tasks, type Choice } from './passingContent'
export type Operation = 'push' | 'try-push' | 'try-rvalue' | 'try-lvalue' | 'pop' | 'try-pop' | 'close' | 'destroy' | 'resume-P' | 'resume-C' | 'finish-P' | 'finish-C' | 'join' | 'other-push' | 'size'
export type Event = Operation | Choice | 'verify' | 'next'
export type Phase = 'ready' | 'waiting' | 'notified' | 'finished'
export interface Item {
  id: number
  payload: 'job' | 'moved-from'
}
export interface Entry {
  op: Operation
  label: string
  result: 'true' | 'false' | 'wait' | 'done' | 'ub'
  reason: string
  owner: 'P' | 'P2' | 'C' | 'O' | null
  itemId?: number
  rejectedBecause?: 'full' | 'closed'
}
export interface QueueState {
  slots: (Item | null)[]
  head: number
  tail: number
  size: number
  closed: boolean
  producer: { phase: Phase; item: Item | null }
  consumer: { phase: Phase }
  mutexOwner: null
  nextId: number
  candidate: Item['payload']
  out: Item | null
  accepted: number[]
  delivered: number[]
  dropped: number[]
  destroyedItems: number[]
  joined: boolean
  destroyed: boolean
  ub: string | null
  log: Entry[]
}
export const operations: readonly Operation[] = ['push', 'try-push', 'try-rvalue', 'try-lvalue', 'pop', 'try-pop', 'close', 'destroy', 'resume-P', 'resume-C', 'finish-P', 'finish-C', 'join', 'other-push', 'size']
export function operationLabel(op: Operation, s: QueueState): string {
  const labels: Record<Operation, string> = {
    push: `P: push(job ${s.nextId})`,
    'other-push': `P2: push(job ${s.nextId})`,
    size: 'P: size()',
    'try-push': `P: try_push(job ${s.nextId})`,
    'try-rvalue': 'P: try_push(std::move(j))',
    'try-lvalue': 'P: try_push(j)',
    pop: 'C: pop(out)',
    'try-pop': 'C: try_pop(out)',
    close: 'O: close()',
    destroy: 'O: destroy the queue',
    'resume-P': `P: resume pending push(job ${s.producer.item?.id ?? '…'})`,
    'resume-C': 'C: resume pending pop(out)',
    'finish-P': 'P: finish all queue use',
    'finish-C': 'C: finish all queue use',
    join: 'O: join both finished users',
  }
  return labels[op]
}

export function simulate(trace: readonly Operation[]): QueueState {
  const s: QueueState = {
    slots: [null, null],
    head: 0,
    tail: 0,
    size: 0,
    closed: false,
    producer: { phase: 'ready', item: null },
    consumer: { phase: 'ready' },
    mutexOwner: null,
    nextId: 1,
    candidate: 'job',
    out: null,
    accepted: [],
    delivered: [],
    dropped: [],
    destroyedItems: [],
    joined: false,
    destroyed: false,
    ub: null,
    log: []
  }
  const notify = (actor: 'producer' | 'consumer') => {
    if (s[actor].phase === 'waiting') s[actor].phase = 'notified'
  }
  for (const op of trace) {
    if (s.ub) throw new Error('Stop at undefined behaviour; no later result is predicted')
    if (s.destroyed) throw new Error('The queue has been destroyed')
    if (!operations.includes(op)) throw new Error('Unknown operation')
    const entry: Entry = { op, label: operationLabel(op, s), result: 'done', reason: '', owner: null }
    if (op === 'other-push') {
      if (s.closed || s.size === 2) throw new Error('The snapshot scene schedules P2 only while one slot is free')
      const item: Item = { id: s.nextId++, payload: 'job' }
      s.slots[s.tail] = item
      s.tail = (s.tail + 1) % 2
      s.size++
      s.accepted.push(item.id)
      notify('consumer')
      entry.owner = 'P2'
      entry.itemId = item.id
      entry.result = 'true'
      entry.reason = 'P2 fills the slot after P’s size() unlocked; increment size and notify not_empty_.'
    } else if (op === 'size') {
      if (s.producer.phase !== 'ready') throw new Error('P cannot start size() during a pending call or after finishing')
      entry.owner = 'P'
      entry.reason = `Read size = ${s.size} under mutex_, then unlock. This reserves no capacity.`
    } else if (['push', 'try-push', 'try-rvalue', 'try-lvalue', 'resume-P'].includes(op)) {
      const resume = op === 'resume-P'
      if (resume ? s.producer.phase !== 'notified' : s.producer.phase !== 'ready') {
        throw new Error(s.producer.phase === 'finished' ? 'P finished all queue use; reset to use it again.' : s.producer.phase === 'ready' ? 'P has no pending push. Fill the queue and call push to make it wait.' : s.producer.phase === 'waiting' ? 'P is asleep in not_full_. Let C pop or O close to notify it.' : 'P has a notified pending push. Use P’s resume button before another call.')
      }
      entry.owner = 'P'
      const item = resume ? s.producer.item! : { id: s.nextId++, payload: op === 'try-rvalue' || op === 'try-lvalue' ? s.candidate : 'job' as const }
      entry.itemId = item.id
      if (op === 'try-rvalue') s.candidate = 'moved-from'
      if (s.closed || (s.size === 2 && !resume && op !== 'push')) {
        entry.rejectedBecause = s.closed ? 'closed' : 'full'
        s.dropped.push(item.id)
        s.producer = { phase: 'ready', item: null }
        entry.result = 'false'
        entry.reason = `${s.closed ? 'closed is true' : 'size == capacity (2)'}; the by-value parameter for item ${item.id} is discarded.`
      } else if (s.size === 2) {
        s.producer = { phase: 'waiting', item }
        entry.result = 'wait'
        entry.reason = 'size == capacity; P atomically releases mutex_ and waits in not_full_. Its parameter still owns the job.'
      } else {
        s.slots[s.tail] = item
        s.tail = (s.tail + 1) % 2
        s.size++
        s.accepted.push(item.id)
        s.producer = { phase: 'ready', item: null }
        notify('consumer')
        entry.result = 'true'
        entry.reason = `Enqueue item ${item.id}${item.payload === 'moved-from' ? ' (moved-from payload)' : ''}; advance tail, increment size, unlock, notify_one(not_empty_).`
      }
    } else if (op === 'pop' || op === 'try-pop' || op === 'resume-C') {
      const resume = op === 'resume-C'
      if (resume ? s.consumer.phase !== 'notified' : s.consumer.phase !== 'ready') {
        throw new Error(s.consumer.phase === 'finished' ? 'C finished all queue use; reset to use it again.' : s.consumer.phase === 'ready' ? 'C has no pending pop. Call pop on an empty, open queue to make it wait.' : s.consumer.phase === 'waiting' ? 'C is asleep in not_empty_. Let P push or O close to notify it.' : 'C has a notified pending pop. Use C’s resume button before another call.')
      }
      entry.owner = 'C'
      if (s.size > 0) {
        const item = s.slots[s.head]!
        entry.itemId = item.id
        s.out = item
        s.delivered.push(item.id)
        s.slots[s.head] = null
        s.head = (s.head + 1) % 2
        s.size--
        s.consumer.phase = 'ready'
        notify('producer')
        entry.result = 'true'
        entry.reason = `Deliver item ${item.id}; advance head, decrement size, unlock, notify_one(not_full_).`
      } else if (s.closed || op === 'try-pop') {
        s.consumer.phase = 'ready'
        entry.result = 'false'
        entry.reason = `${s.closed ? 'Closed and drained' : 'Empty right now'}; out is unchanged.`
      } else {
        s.consumer.phase = 'waiting'
        entry.result = 'wait'
        entry.reason = 'size == 0 and closed == false; C atomically releases mutex_ and waits in not_empty_.'
      }
    } else if (op === 'close') {
      entry.owner = 'O'
      entry.reason = s.closed ? 'Already closed: idempotent return; no additional notification.' : 'Set closed under mutex_, unlock, notify_all(not_full_) and notify_all(not_empty_). Waiters still need to resume.'
      if (!s.closed) {
        s.closed = true
        notify('producer')
        notify('consumer')
      }
    } else if (op === 'finish-P' || op === 'finish-C') {
      const actor = op === 'finish-P' ? s.producer : s.consumer
      if (actor.phase !== 'ready') throw new Error('A pending call must return before its user can finish')
      actor.phase = 'finished'
      entry.reason = 'This thread finishes; it will make no more queue accesses.'
    } else if (op === 'join') {
      if (s.producer.phase !== 'finished' || s.consumer.phase !== 'finished' || s.joined) throw new Error('Let both users finish first; then O can join them once')
      s.joined = true
      entry.reason = 'O observes both thread completions by joining. The queue can now be destroyed.'
    } else {
      if (s.producer.phase !== 'finished' || s.consumer.phase !== 'finished') {
        s.ub = 'Undefined behaviour: the queue is destroyed before every user has finished; a notified call is still pending.'
        entry.result = 'ub'
        entry.reason = s.ub
      } else {
        s.destroyedItems = s.slots.flatMap(item => item ? [item.id] : [])
        s.slots = [null, null]
        s.size = 0
        s.destroyed = true
        entry.reason = 'Every user has finished. Destruction releases storage, including any undrained items; it delivers no work.'
      }
    }
    s.log.push(entry)
  }
  return s
}
export function blockedReason(trace: readonly Operation[], op: Operation): string | null {
  try {
    simulate([...trace, op])
    return null
  } catch (e) {
    return (e as Error).message
  }
}
export type Space = 'full' | 'empty' | 'pending-push' | 'pending-pop' | 'close-push' | 'close-pop'
export interface Ordering {
  trace: Operation[]
  state: QueueState
  reason: string
}
// Fixed prefixes define the small spaces. Permute the remaining operations, retaining
// only legal schedules; resume is inserted only for a pending, notified call.
export function enumerateOrderings(policy: 'blocking' | 'trying', space: Space): Ordering[] {
  const send: Operation = policy === 'blocking' ? 'push' : 'try-push'
  if (space === 'full') return [
    ['push', 'push', send, 'pop', ...(policy === 'blocking' ? ['resume-P' as const] : [])],
    ['push', 'push', 'pop', send],
  ].map(trace => row(trace as Operation[]))
  if (space === 'empty') return [row(['pop', 'push', 'resume-C']), row(['push', 'pop'])]
  const pending = space === 'pending-push' || space === 'pending-pop'
  const pushing = space === 'pending-push' || space === 'close-push'
  const prefix: Operation[] = pushing ? ['push', 'push', ...(pending ? ['push' as const] : [])] : pending ? ['pop'] : []
  const remaining: Operation[] = pending ? (pushing ? ['pop', 'close', 'resume-P'] : ['push', 'close', 'resume-C']) : [pushing ? send : 'pop', 'close']
  const rows: Ordering[] = []
  const walk = (trace: Operation[], todo: Operation[]) => {
    if (!todo.length) {
      const s = simulate(trace)
      const resume: Operation | null = s.producer.phase === 'notified' ? 'resume-P' : s.consumer.phase === 'notified' ? 'resume-C' : null
      rows.push(row(resume ? [...trace, resume] : trace))
      return
    }
    for (let i = 0; i < todo.length; i++) {
      const next = [...trace, todo[i]]
      if (blockedReason(trace, todo[i])) continue
      walk(next, todo.filter((_, j) => j !== i))
    }
  }
  walk(prefix, remaining)
  return rows
}
function row(trace: Operation[]): Ordering {
  const state = simulate(trace)
  const wait = state.log.find(e => e.result === 'wait')
  const rejection = state.log.find(e => e.result === 'false' && ['push', 'try-push', 'resume-P'].includes(e.op))
  const resumed = state.log.find(e => e.op === 'resume-C')
  const reason = rejection ? `${wait ? 'The pending call wakes and rechecks. ' : ''}${rejection.reason}` : wait ? `${wait.reason} Notification makes resumption possible; the resumed call rechecks and completes.` : 'The item or free slot is already present when the call acquires the mutex; no capacity/item wait is needed.'
  return {
    trace,
    state,
    reason: reason + (resumed ? ` C’s pending pop returns ${resumed.result}${resumed.itemId ? ` with item ${resumed.itemId}` : ' because the queue is closed and drained'}.` : '')
  }

}

export interface WorkshopState {
  task: number
  trace: Operation[]
  reference: Operation[]
  evidence: Choice | null
  verdict: { ok: boolean; explanation: string } | null
  completed: number[]
  taskStart: number
}
export const initialTrace = (task: number): Operation[] => task === 2 || task === 3 ? ['push', 'push'] : task === 4 ? ['finish-P', 'pop'] : task === 5 ? ['push'] : []
export const guided: Record<number, Event[]> = {
  0: ['push', 'push', 'push', 'pop', 'resume-P'],
  1: ['pop', 'push', 'resume-C'],
  2: ['ownership-husk'],
  3: ['close', 'push', 'pop', 'pop', 'pop'],
  4: ['lifetime-unsafe'],
  5: ['snapshot-blocks'],
}
export function evaluate(s: WorkshopState): { ok: boolean; explanation: string } {
  if (taskChoices[s.task]) {
    if (!s.evidence) throw new Error('Select an outcome before checking')
    return { ok: s.evidence === correct[s.task], explanation: choices[s.evidence].feedback }
  }
  const sim = simulate(s.trace)
  const waitP = sim.log.findIndex(e => e.op === 'push' && e.result === 'wait' && e.itemId === 3)
  const waitC = sim.log.findIndex(e => e.op === 'pop' && e.result === 'wait')
  if (s.task === 0) {
    const ok = waitP >= 0 && sim.log.some((e, i) => i > waitP && e.op === 'resume-P' && e.result === 'true' && e.itemId === 3) && sim.accepted.includes(3) && sim.delivered.includes(1) && !sim.ub
    return {
      ok,
      explanation: ok ? 'P waited because size == capacity. C’s pop advanced head, decremented size and notified not_full_. P then reacquired mutex_, rechecked, wrote job 3 into the freed slot, advanced tail and incremented size. Head, tail, size, closed and the slots stayed protected throughout.' : 'Not yet: demonstrate P sleeping on full, C freeing a slot, then P resuming that pending push. Filling the queue alone does not show the wait. Reset task if you closed or destroyed it.'
    }
  }
  if (s.task === 1) {
    const ok = waitC >= 0 && sim.log.some((e, i) => i > waitC && e.op === 'resume-C' && e.result === 'true' && e.itemId === 1) && sim.delivered.includes(1) && !sim.ub
    return {
      ok,
      explanation: ok ? 'C waited because size == 0 and closed was false. P’s push wrote job 1, advanced tail, incremented size and notified not_empty_. C then reacquired mutex_, rechecked, moved job 1 to out, advanced head and decremented size. The same mutex protects head, tail, size, closed and the slots.' : 'Not yet: start with C asleep in an empty, open queue, push a job, then resume C’s pending pop. Notification alone does not deliver an item.'
    }
  }
  const ok = sim.closed && sim.size === 0 && sim.delivered.join(',') === '1,2' && sim.log.some(e => e.itemId === 3 && e.rejectedBecause === 'closed' && (e.op === 'push' || e.op === 'resume-P')) && sim.log.some(e => (e.op === 'pop' || e.op === 'resume-C') && e.result === 'false') && !sim.ub
  return {
    ok,
    explanation: ok ? 'Close is a one-way switch: it refuses P’s job 3 and lets C drain jobs 1 then 2. It does not empty the queue or join anyone. C’s final pop returned false because closed was true and size was zero. Every legal ordering meeting those conditions earns credit.' : 'Not yet: end closed and drained with jobs 1 and 2 delivered, job 3 refused, and a blocking pop returning false. Closing alone does not drain; an empty open pop waits. Reset task if job 3 was accepted.'
  }
}
export function replay(events: readonly Event[]): WorkshopState {
  if (events.length > HISTORY_LIMIT) throw new Error('History too long')
  const s: WorkshopState = { task: 0, trace: [], reference: [], evidence: null, verdict: null, completed: [], taskStart: 0 }
  for (const [index, event] of events.entries()) {
    if (event === 'next') {
      if (!s.verdict?.ok || s.task >= tasks.length - 1) throw new Error('Check this question before advancing')
      if (s.task === 0) s.reference = [...s.trace]
      s.completed.push(s.task++)
      s.trace = initialTrace(s.task)
      s.evidence = null
      s.verdict = null
      s.taskStart = index + 1
    } else {
      if (s.verdict?.ok) throw new Error('Checked tasks are read-only; advance or reset')
      if (event === 'verify') s.verdict = evaluate(s)
      else if (operations.includes(event as Operation)) {
        if (taskChoices[s.task] || event === 'other-push' || event === 'size') throw new Error('This checkpoint asks for a prediction')
        simulate([...s.trace, event as Operation])
        s.trace.push(event as Operation)
        s.verdict = null
      } else if (taskChoices[s.task]?.includes(event as Choice)) {
        s.evidence = event as Choice
        s.verdict = null
      }
      else throw new Error('Unknown action or answer for this checkpoint')
    }
  }
  return s
}
export const eventsThroughPassingTask = (events: readonly Event[], task: number) => eventsThroughTask(events, task)
export const href = (events: readonly Event[]) => lessonHref('passing-an-item', '1', events)
export const readLocation = (url: string) => readLessonLocation<Event>('passing-an-item', '1', url, replay)
