import { simulate, type Operation, type QueueState } from '../lessons/passingWorkshop'

const phaseText = (phase: string, cv: string) => phase === 'waiting' ? `asleep in ${cv}` : phase === 'notified' ? `notified from ${cv}; awaiting resume` : phase === 'finished' ? 'finished all queue use' : 'ready for a call'
export function QueueDiagram({ state: s, compact = false }: { state: QueueState; compact?: boolean }) {
  return <figure className={`queue-figure ${s.ub ? 'queue-ub' : ''} ${compact ? 'queue-compact' : ''}`}>
    <div className="queue-board" role="img" aria-label={`Two slots: ${s.slots.map((item, i) => `slot ${i}: ${item ? `item ${item.id}, ${item.payload}` : 'empty'}`).join('; ')}. size ${s.size}, closed ${s.closed}. P ${s.producer.phase}, C ${s.consumer.phase}${s.ub ? '. Undefined behaviour' : ''}`}>
      <p className="queue-board-title">{s.ub ? 'STOP · undefined behaviour' : s.destroyed ? 'Storage destroyed' : 'cq::MutexQueue<Job> · capacity 2'}</p>
      <div className="queue-slots">{s.slots.map((item, i) => <div key={i} className={item ? 'queue-slot occupied' : 'queue-slot'}>
        <span>Slot {i}</span><strong>{item ? `Item ${item.id}` : 'Empty'}</strong>
        {item && <span>{item.payload === 'moved-from' ? 'Moved-from payload' : 'Job payload'}</span>}
        <small>{s.head === i ? 'head → next read' : ''}</small><small>{s.tail === i ? 'tail → next write' : ''}</small>
      </div>)}</div>
      <p className="queue-indices">size = {s.size} / 2 · head = {s.head} · tail = {s.tail}<br />closed = {String(s.closed)}</p>
      <div className="queue-waitsets"><p><strong>P:</strong> {phaseText(s.producer.phase, 'not_full_')}</p><p><strong>C:</strong> {phaseText(s.consumer.phase, 'not_empty_')}</p></div>
    </div>
    <figcaption>The slot identities show where storage is reused; the two wait sets show who can resume. Both redraw after each selected step. Empty means logically unoccupied; C++ keeps a moved-from T in that storage.</figcaption>
  </figure>
}
export function QueueTimeline({ trace, id = 'queue-timeline' }: { trace: readonly Operation[]; id?: string }) {
  const s = simulate(trace)
  const actors = trace.includes('other-push') ? ['P', 'P2', 'C', 'O'] : ['P', 'C', 'O']
  return <div className="queue-timeline" role="group" aria-label="Operation timeline" tabIndex={0}>
    {!trace.length ? <p>No operations yet. Choose an actor below.</p> : <>
      <svg viewBox={`0 0 ${Math.max(420, trace.length * 66 + 40)} ${actors.length * 31 + 7}`} width={Math.max(420, trace.length * 66 + 40)} height={actors.length * 31 + 7} role="img" aria-labelledby={`${id}-title`}>
        <title id={`${id}-title`}>Thread order: numbered steps align with the operation log below</title>
        {actors.map((actor, i) => <g key={actor}><text x="2" y={22 + i * 31}>{actor}</text><line x1="25" y1={17 + i * 31} x2={trace.length * 66 + 30} y2={17 + i * 31} /></g>)}
        {s.log.map((entry, i) => {
          const actor = entry.owner ?? 'P'
          const y = 17 + actors.indexOf(actor) * 31
          return <g key={i}><circle cx={54 + i * 66} cy={y} r="12" /><text x={54 + i * 66} y={y + 4} textAnchor="middle" className="timeline-number">{i + 1}</text></g>
        })}
      </svg>
      <ol>{s.log.map((entry, i) => <li key={i}><strong>{entry.label} → {entry.result}</strong><span>{entry.reason}</span></li>)}</ol>
    </>}
    <p className="booking-legend">The lanes show the order you chose, not elapsed time or parallel execution. Numbers identify the corresponding operation below.</p>
  </div>
}
