import { useState } from 'react'
import { annotations, choices, guides, taskChoices, tasks } from '../lessons/passingContent'
import { blockedReason, eventsThroughPassingTask, guided, href, initialTrace, operationLabel, readLocation, replay, simulate, type Event, type Operation } from '../lessons/passingWorkshop'
import { header, implementation, lifetimeContract, ownershipContract, snapshotContract, sourceCode, taskCode } from '../lessons/passingSource'
import { useWorkshopHistory } from './useWorkshopHistory'
import { CodeListing, Controls, FullExample, GuidedStep, HowTo, LockedPreview, ProgressStrip, QuestionHeader, RESET_TASK_NOTICE, RESTART_NOTICE, SIMULATION_NOTE, SituationPanel, Takeaway } from './WorkshopShell'
import { QueueDiagram, QueueTimeline } from './QueueDiagram'
import { PassingDryRun } from './PassingDryRun'
import '../styles/booking.css'

const situation = {
  setup: [
    {
      term: 'Meaning of the shared thing',
      detail: 'A bounded first-in, first-out handoff stores up to two jobs between calls: cq::MutexQueue<Job> q(2). The two reusable positions form a ring.'
    },
    {
      term: 'Scenario',
      detail: 'Producer P submits jobs 1, 2, 3…; Consumer C removes them; Owner O controls close and lifetime. A mutex gives one call at a time access to the slots, head, tail, size and closed flag. A condition variable (CV) lets a thread sleep while releasing that mutex; not_full_ and not_empty_ are the two places to wait.'
    },
    {
      term: 'Rule',
      detail: 'Accepted jobs stay queued until removed in order, or storage is destroyed after all users finish. Jobs in this scene have nonthrowing moves. Transferring a job does not mean its work has been completed.'
    },
    {
      term: 'Failure',
      detail: 'A failed try_push can discard its argument; a sleeping user can outlive storage unless the owner finishes and joins every user before destruction.'
    },
  ],
  looksSafe: 'It is tempting to treat a false return as “my argument is untouched”, or close as “everyone has finished”. The source promises neither.',
  whenToUse: [
    {
      pattern: 'push / pop: wait for a condition',
      meaning: 'Sleep on full / empty-and-open.',
      rightWhen: 'You want a caller to wait for room or work without polling.',
      wrongWhen: 'The caller must return now or no actor can make the condition change.',
      bug: 'A correct wait can last forever if nobody pops, pushes or closes.'
    },
    {
      pattern: 'try_push / try_pop: omit that wait',
      meaning: 'Same mutex and storage; return false on full / empty.',
      rightWhen: 'The caller has other work or an explicit rejection policy.',
      wrongWhen: 'A retry assumes a moved argument survives rejection.',
      bug: 'The next attempt may enqueue a moved-from object; a tight retry loop burns CPU.'
    },
    {
      pattern: 'close, finish, join, destroy',
      meaning: 'Refuse new work, wake sleepers, finish users, then release storage.',
      rightWhen: 'The owner is ending the handoff.',
      wrongWhen: 'The owner treats notification as completion.',
      bug: 'A pending call accesses a destroyed queue: undefined behaviour.'
    },
  ],
  anchors: [{
    kind: 'Repository regression witness',
    domain: 'Rejected task ownership',
    text: 'FailedPushConsumesRvaluesAndLeavesLvaluesIntact reproduces a full queue consuming a unique_ptr even when try_push returns false. Retrying that object would submit a hollow payload.',
    source: 'tests/mutex_queue_test.cpp:208–230 at 8c68e32. This is a regression demonstration, not a claimed production incident.'
  }]
}

function BeforeTheQueue() {
  const [need, setNeed] = useState(0)
  const needs = [
    {
      title: 'Somewhere for work to wait',
      scene: 'Sender finishes job 1 → job 1 waits → receiver is still busy',
      why: 'The threads run at different speeds. A completed job needs a place to stay until the receiver is ready.'
    },
    {
      title: 'An idle receiver that does not burn a core',
      scene: 'No jobs waiting → receiver asleep → CPU can do other work',
      why: 'Repeatedly asking “anything yet?” does no useful work. The receiver needs a way to sleep until the situation changes.'
    },
    {
      title: 'A limit when the sender gets ahead',
      scene: 'Job 1 waiting · job 2 waiting → job 3: sender must wait or be refused',
      why: 'A slow receiver must not mean unlimited memory growth. A full handoff needs backpressure: slow or reject the sender.'
    },
    {
      title: 'Wake sleepers when work ends',
      scene: 'No more jobs coming → wake receiver → receiver learns it can finish',
      why: 'A sleeping receiver needs a shutdown signal even when there will never be another job.'
    },
  ]
  return <section className="queue-motivation" aria-labelledby="problem-title"><p className="kicker">Start with the handoff</p><h2 id="problem-title">Two threads, different speeds</h2><p>One thread makes work. Another handles it later. What must happen in between?</p>
    <div className="needs-grid">{needs.map((n, i) => <button key={n.title} aria-pressed={need === i} onClick={() => setNeed(i)}><span>Need {i + 1}</span><strong>{n.title}</strong><small>{n.scene}</small></button>)}</div>
    <div className="need-scene" role="status" aria-label="Handoff need"><strong>{needs[need].scene}</strong><p>{needs[need].why}</p></div>
    <p className="booking-legend">Select a need to follow the waiting job or thread. These scenes show the problem before choosing storage or synchronization.</p>
    <h3>Names for what you just saw</h3><dl className="queue-vocabulary">
      <div><dt>Producer and consumer</dt><dd>A producer makes or submits work; a consumer removes work to handle it. They are roles played by threads.</dd></div>
      <div><dt>How many of each?</dt><dd>SPSC means single producer, single consumer. MPMC means multiple producers, multiple consumers. This scene uses SPSC; this queue also supports MPMC.</dd></div>
      <div><dt>Concurrent queue</dt><dd>A handoff that several threads may push into and pop from safely. “Concurrent” allows interleaving on one CPU; it does not require parallel execution.</dd></div>
      <div><dt>Blocking queue</dt><dd>A waiting policy, not a different storage structure: pop sleeps while empty and open; bounded push sleeps while full and open. The try_ calls use the same queue with the capacity/item wait removed.</dd></div>
      <div><dt>Non-blocking / lock-free queue</dt><dd>A queue designed without sleeping on locks or waiting for space or items: empty or full returns “nothing now” for the caller to handle. Lock-free is a progress guarantee, not a claim made by a try_ name. These queues are not built yet in this repository. MutexQueue’s try_ calls still acquire a mutex and may wait for that lock.</dd></div>
    </dl></section>
}
const available: Record<string, readonly Operation[]> = { P: ['push', 'try-push', 'resume-P', 'finish-P'], C: ['pop', 'try-pop', 'resume-C', 'finish-C'], O: ['close', 'join', 'destroy'] }

export function PassingWorkshop() {
  const { location, notice, viewing, setViewing, navigate, act } = useWorkshopHistory(readLocation, href, replay)
  const live = replay(location.events), readOnly = viewing !== null && viewing < live.task
  const state = readOnly ? replay(eventsThroughPassingTask(location.events, viewing)) : live
  const preview = viewing !== null && viewing > live.task ? viewing : null
  const task = tasks[state.task], guide = guides[state.task], answering = !!taskChoices[state.task]
  const reference = state.task === 2 ? state.reference.slice(0, state.reference.findIndex((op, i) => op === 'push' && simulate(state.reference.slice(0, i + 1)).producer.phase === 'waiting') + 1) : state.trace
  const revealed: Operation[] = state.task === 2 ? [...initialTrace(2), 'try-rvalue', 'pop', 'try-rvalue'] : state.task === 4 ? [...initialTrace(4), 'close', 'destroy'] : state.task === 5 ? ['push', 'size', 'other-push', 'push'] : state.trace
  const visible = answering ? (state.verdict ? revealed : reference) : state.trace
  const sim = simulate(visible), current = simulate(state.trace)
  const canVerify = answering ? !!state.evidence : state.trace.length > initialTrace(state.task).length
  const canUndo = location.events.length > live.taskStart
  const hint = [state.verdict?.ok ? 'This task is checked; Check this task is disabled. Continue or reset to explore again.' : !canVerify ? (answering ? 'Select an answer above to enable Check this task.' : 'Run an actor operation above to enable Check this task.') : 'Check your trace or answer. Change an incorrect answer, add steps, or reset the task.', state.task < 5 && !state.verdict?.ok ? 'Next question unlocks after a passing check.' : '', !canUndo ? 'Previous is disabled at the start of this task; use the actor controls or choose an answer.' : 'Previous undoes one event in this task.'].filter(Boolean).join(' ')
  function nextGuided(): Operation | null {
    if (answering || state.verdict?.ok || sim.ub || sim.destroyed) return null
    const additions = state.trace.slice(initialTrace(state.task).length)
    const order = guided[state.task]
    if (additions.every((op, i) => op === order[i])) return (order[additions.length] as Operation) ?? null
    // Find a shortest continuation satisfying the objective, instead of silently resetting
    // a learner's legal alternative. Small bounded search, only the relevant operations.
    const candidates: Operation[] = state.task === 0 ? ['push', 'pop', 'resume-P'] : state.task === 1 ? ['pop', 'push', 'resume-C'] : ['close', 'push', 'pop', 'resume-P', 'resume-C']
    const queue: Operation[][] = [[]]
    while (queue.length) {
      const suffix = queue.shift()!
      if (suffix.length >= 6) continue
      for (const op of candidates) {
        if (blockedReason(state.trace.concat(suffix), op)) continue
        const next = [...suffix, op]
        try {
          const checked = replay([...location.events, ...next, 'verify'])
          if (checked.verdict?.ok) return next[0]
        } catch {
          continue
        }
        queue.push(next)
      }
    }
    return null
  }
  const guidedNext = readOnly ? null : nextGuided()
  const lastOp = sim.log.at(-1)?.op
  const activeFunction = lastOp === 'resume-C' || lastOp === 'pop' ? 'MutexQueue<T>::pop' : lastOp === 'try-pop' ? 'MutexQueue<T>::try_pop' : lastOp === 'try-push' || lastOp === 'try-rvalue' || lastOp === 'try-lvalue' ? 'MutexQueue<T>::try_push' : lastOp === 'close' ? 'MutexQueue<T>::close' : lastOp === 'size' ? 'MutexQueue<T>::size' : lastOp === 'push' || lastOp === 'resume-P' || lastOp === 'other-push' ? 'MutexQueue<T>::push' : null
  let visibleCode = taskCode(state.task)
  if (activeFunction && !visibleCode.includes(activeFunction)) visibleCode = Object.values(sourceCode).find(code => code.includes(activeFunction))! + '\n\n' + visibleCode
  return <main className="booking-lesson workshop queue-lesson" id="lesson"><header className="queue-masthead"><p className="kicker">concurrent-queue / tutorial 01</p><h1>Passing an item</h1><p>Six questions about waiting, ownership, and knowing when a queue can go away.</p><small>Implementation 8c68e32 · unmerged v1 branch · lesson revision 1</small></header>
    <BeforeTheQueue />
    <SituationPanel id="passing-title" kicker="The implementation" title="Two places to leave a job" idea="Choose what the caller should do when it cannot proceed" situation={situation} goal="Explore the handoff below, one operation at a time." anchorNote="This witness provides context. Every graded premise appears beside its question; you do not need to open the test." />
    {location.error && <p role="alert" className="booking-link-error">{location.error}</p>}
    <ProgressStrip tasks={tasks} live={{ task: live.task, completed: live.completed, passed: !!live.verdict?.ok }} viewing={viewing} onView={setViewing} onNext={() => act('next')} />
    {preview !== null ? <LockedPreview id="passing-preview" index={preview} tasks={tasks} liveTask={live.task} reuses="wait-and-resume reasoning" onBack={() => setViewing(null)} /> : <>
      <QuestionHeader index={state.task} total={6} task={task} lead={guide.lead} readOnly={readOnly} />
      <div className="workshop-experiment">
        <div>
          {state.task === 2 && <section className="queue-contract" aria-label="Argument ownership contract"><h4>The signature and its ownership rule</h4><pre tabIndex={0}><code>{ownershipContract}</code></pre><p>The scene’s Job has a move constructor that transfers its payload and leaves the source moved-from; moves do not throw.</p><pre tabIndex={0}><code>{'Job j = make_job();\nwhile (!q.try_push(std::move(j))) {\n  if (q.closed()) break;\n}'}</code></pre></section>}
          {state.task === 4 && <section className="queue-contract"><h4>The documented lifetime rule</h4><pre tabIndex={0}><code>{lifetimeContract}</code></pre><p>Initial state: C is asleep in pop; P has finished. O closes and immediately destroys before C resumes.</p></section>}
          {state.task === 5 && <section className="queue-contract"><h4>What the two calls promise</h4><pre tabIndex={0}><code>{snapshotContract}</code></pre><pre tabIndex={0}><code>{'if (q.size() < q.capacity()) {\n  // Another producer may push here.\n  q.push(x);\n}'}</code></pre></section>}
          <CodeListing code={visibleCode} annotations={annotations} active={activeFunction}>
            <p>Direct slices of <code>include/cq/mutex_queue.ipp</code>. Each operation is one coarse step until it returns or waits. A pending call gets a separate resume step. Mutex ownership is inside the step only; wait releases it and resume reacquires it.</p>
            <details><summary>Slot updates and try_pop source</summary><pre tabIndex={0}><code>{sourceCode.ring + '\n\n' + sourceCode.tryPop}</code></pre></details>
          </CodeListing>
        </div>
        <div className={`booking-stage ${sim.ub ? 'booking-failed' : ''}`} role="group" aria-label="Simulated execution">
          <div className="booking-stage-header"><strong>Producer P · Consumer C · Owner O</strong></div><p className="booking-simulation-note">{SIMULATION_NOTE}</p><p className="booking-goal">{task.goal}</p><HowTo guide={guide} />
          {answering && !state.verdict && <div className="queue-reference"><h4>Reason before revealing</h4><p>{state.task === 2 ? 'Reference: your original full-queue wait. It deliberately stays unchanged while you predict replacing that call with repeated try_push(std::move(j)). Checking redraws rejection, C freeing a slot, and the retry.' : state.task === 4 ? 'Reference: C’s pending pop, before O closes. It deliberately stays unchanged while you reason. Checking shows O close and destroy before C resumes.' : 'Reference: one occupied slot when P observes size() == 1. It deliberately stays unchanged until you check; the reveal shows the other producer filling that slot before P’s push.'}</p></div>}
          {!answering && guidedNext && <GuidedStep first={state.trace.length === initialTrace(state.task).length} onRun={() => act(guidedNext)} />}
          {!answering && !state.verdict?.ok && !guidedNext && !readOnly && <p className="booking-legend">No guided continuation meets this task from here. Use Reset task to try again.</p>}
          <QueueDiagram state={sim} />
          {state.task === 5 && state.verdict && <p className="queue-reference">P2 is the second producer in this changed scene. The timeline shows its enqueue between P’s size() and push(); the diagram and ledger now show that same run.</p>}
          <div className="queue-ledger"><span>Accepted: {sim.accepted.join(', ') || 'none'}</span><span>Delivered to C: {sim.delivered.join(', ') || 'none'}</span><span>Rejected / dropped: {sim.dropped.join(', ') || 'none'}</span><span>Destroyed undrained: {sim.destroyedItems.join(', ') || 'none'}</span></div>
          <p className="booking-lock">Mutex owner during the last step: <strong>{sim.log.at(-1)?.owner ?? 'none'}</strong>. Between steps: <strong>none</strong>. {sim.producer.phase === 'waiting' || sim.consumer.phase === 'waiting' ? 'Sleeping threads hold no mutex.' : ''}</p>
          {sim.ub && <p className="workshop-stop">{sim.ub} No later outcome is predicted.</p>}
          <QueueTimeline trace={visible} />
          {!answering && <div className="queue-actors">{Object.entries(available).map(([actor, ops]) => <section key={actor} aria-label={`${actor === 'P' ? 'Producer P' : actor === 'C' ? 'Consumer C' : 'Owner O'} operations`}><h4>{actor === 'P' ? 'Producer P' : actor === 'C' ? 'Consumer C' : 'Owner O'}</h4>{ops.map(op => {
            const reason = readOnly ? 'Checked question: read-only. Return to the current question.' : state.verdict?.ok ? 'Task checked. Continue or reset.' : blockedReason(state.trace, op)
            return <div key={op}><button disabled={!!reason} aria-describedby={`reason-${op}`} onClick={() => act(op)}>{operationLabel(op, current)}</button><small id={`reason-${op}`}>{reason ?? (op === 'destroy' ? 'Runs destruction now; the model stops if any user has not finished.' : op.startsWith('finish') ? 'Finish this thread only after its current call returns.' : 'You choose when this operation runs.')}</small></div>
          })}</section>)}</div>}
        </div></div>
      {answering && <fieldset className="queue-choices" disabled={readOnly || !!state.verdict?.ok}><legend>Choose the outcome and its cause</legend>{taskChoices[state.task].map(choice => <label key={choice}><input type="radio" name="prediction" value={choice} checked={state.evidence === choice} onChange={() => act(choice)} /><span>{choices[choice].text}</span><small>{state.evidence === choice ? 'selected' : 'select'}</small></label>)}{(readOnly || state.verdict?.ok) && <p>This answer is checked and read-only. Continue or reset to answer again.</p>}</fieldset>}
      <p role="status" aria-label="Task feedback" className="booking-feedback">{state.verdict?.explanation ?? 'Build your trace or select an outcome, then check to see the causal explanation.'}</p>
      {notice && <p role="status">{notice}</p>}
      <Controls readOnly={readOnly} liveTask={live.task} onBack={() => setViewing(null)} canVerify={canVerify} verdictOk={!!state.verdict?.ok} hasVerdict={!!state.verdict?.ok} hasNext={state.task < 5} hint={hint} canUndo={canUndo} onVerify={() => act('verify')} onNext={() => act('next')} onUndo={() => navigate(location.events.slice(0, -1))} onResetTask={() => navigate(location.events.slice(0, live.taskStart), RESET_TASK_NOTICE)} onRestart={() => navigate([], RESTART_NOTICE)} />
      {live.completed.includes(0) || (live.task === 0 && live.verdict?.ok) ? <PassingDryRun task={state.task} /> : <p className="booking-legend">The exhaustive dry run unlocks after question 1 is checked. Build your own wait-and-resume trace first.</p>}
      {state.task === 5 && state.verdict?.ok && <Takeaway total={6} summary="A queue transfers jobs. Waiting, owning a payload, closing, and finishing its users are separate responsibilities." prompt="Explain why notification did not authorize destruction. Then explain how a failed try_push changes the caller’s ownership and how you would submit an irreplaceable job." checks="Look for: predicate recheck under the mutex; a by-value move before rejection; close refuses and drains; join establishes completion before storage ends. These automated checks do not establish learner comprehension." />}
      <FullExample source={implementation}><p>Real header-only implementation, imported directly from the repository at 8c68e32. Job moves in this model do not throw. The actual template’s exception and type restrictions are in the header below. No abort, automatic join, fairness or lock-free guarantee is provided.</p><details><summary>Full API header and restrictions</summary><pre tabIndex={0}><code>{header}</code></pre></details></FullExample>
    </>}
    <footer className="queue-footer"><p>Local simulation · no C++ runs in your browser. The independent C++ witness is in tutorials/examples/q1_passing_an_item.cpp. This tutorial does not change the library build.</p><a href="https://github.com/jadecubes/concurrent-queue/blob/8c68e32/include/cq/mutex_queue.hpp">Pinned API source</a></footer>
  </main>
}
