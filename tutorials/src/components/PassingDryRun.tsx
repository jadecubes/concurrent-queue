import { useState } from 'react'
import { enumerateOrderings, simulate, type Ordering, type Space } from '../lessons/passingWorkshop'
import { QueueDiagram } from './QueueDiagram'
import { LazyDetails } from './WorkshopShell'
function Run({ row, index }: { row: Ordering; index: number }) {
  const [step, setStep] = useState(row.trace.length)
  const s = simulate(row.trace.slice(0, step))
  return <article className="dry-run-row"><h5>Ordering {index + 1}: {row.state.log.map(e => e.label).join(' → ')}</h5><p>{row.reason}</p>
    <QueueDiagram state={s} compact />
    <p>Step {step} of {row.trace.length}: {s.log.at(-1)?.reason ?? 'Empty, open queue before the fixed starting prefix.'}</p>
    <div className="dry-controls"><button disabled={step === 0} onClick={() => setStep(step - 1)}>Previous diagram step</button><button disabled={step === row.trace.length} onClick={() => setStep(step + 1)}>Next diagram step</button></div>
    <small>{step === 0 ? 'At the beginning; use Next diagram step.' : step === row.trace.length ? 'At the end; use Previous diagram step to inspect the ordering.' : 'Each control changes only this ordering’s diagram.'}</small>
  </article>
}
const labels: Record<Space, string> = {
  full: 'Full: third push versus one pop',
  empty: 'Empty: pop versus the first push',
  'pending-push': 'Full, P already asleep: one pop, close, and P’s resumption',
  'pending-pop': 'Empty, C already asleep: one push, close, and C’s resumption',
  'close-push': 'Full: close versus a new push',
  'close-pop': 'Empty: close versus a new pop'
}
// Enumerated once: each space depends on the queue's code, not on anything the learner does.
const spaces = new Map<string, ReturnType<typeof enumerateOrderings>>()
const rowsFor = (policy: 'blocking' | 'trying', space: Space) => {
  const key = `${policy}-${space}`
  if (!spaces.has(key)) spaces.set(key, enumerateOrderings(policy, space))
  return spaces.get(key)!
}

function SpaceRows({ space }: { space: Space }) {
  const policies = space === 'full' || space === 'close-push' ? ['blocking', 'trying'] as const : ['blocking'] as const
  return <section><h4>{labels[space]}</h4>{policies.map(policy => <div key={policy}><p><strong>{policy === 'blocking' ? 'Blocking push/pop' : 'try_push (no capacity wait)'}</strong></p>{rowsFor(policy, space).map((row, i) => <Run key={`${space}-${policy}-${i}`} row={row} index={i} />)}</div>)}</section>
}
export function PassingDryRun({ task }: { task: number }) {
  const focused: Space = task === 1 ? 'empty' : task === 3 ? 'pending-push' : task === 4 ? 'pending-pop' : 'full'
  return <section className="queue-dry-run" aria-label="Exhaustive dry run"><h3>Compare the small scheduling spaces</h3>
    <p>Unlocked by your first wait-and-resume trace. For the full scene, jobs 1 and 2 are already queued: the only new calls are P’s third push and C’s one pop. Pending-call scenes add one close and one explicit resumption. These are all legal orders in that stated space; each diagram can be stepped backward and forward.</p>
    <SpaceRows key={focused} space={focused} />
    <LazyDetails summary="Compare all: full, empty, close, and pending calls">
      {() => <>{(Object.keys(labels) as Space[]).filter(space => space !== focused).map(space => <SpaceRows key={space} space={space} />)}</>}
    </LazyDetails>
    <p className="booking-legend">This operation-level model omits spurious wakes, exceptions and interleavings between unlock and notify. It makes no fairness or timing guarantee and is not an enumeration of all C++ executions.</p>
  </section>
}
