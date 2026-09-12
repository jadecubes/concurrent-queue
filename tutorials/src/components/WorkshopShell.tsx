import { Fragment, type ReactNode } from 'react'

// Presentational pieces shared by every workshop lesson. They hold the page structure the
// learner review settled on (CLAUDE.md, "Page structure the user accepted"); lessons supply words.

export interface Situation {
  setup: readonly { term: string; detail: string }[]
  looksSafe: string
  whenToUse: readonly { pattern: string; meaning: string; rightWhen: string; wrongWhen: string; bug: string }[]
  anchors: readonly { kind: string; domain: string; text: string; source: string }[]
}
export interface Task {
  title: string
  goal: string
  question: string
  instruction: string
}
export interface Guide {
  lead: string
  howToLabel: 'How to play' | 'How to answer'
  howTo: readonly string[]
}

export function SituationPanel({ id, kicker, title, idea, situation, goal, anchorNote }: { id: string; kicker: string; title: string; idea: string; situation: Situation; goal: string; anchorNote: string }) {
  return <>
    <header className="booking-intro">
      <p className="kicker">{kicker}</p>
      <h2 id={id}>{title}</h2>
      <ul className="workshop-setup" aria-label="Setup">
        {situation.setup.map(({ term, detail }) => <li key={term}><strong>{term}</strong><span>{detail}</span></li>)}
      </ul>
    </header>
    <section className="workshop-situation" aria-labelledby={`${id}-idea`}>
      <h3 id={`${id}-idea`}>{idea}</h3>
      <table className="workshop-guide" aria-label="When to use which">
        <thead><tr><th scope="col">Pattern and what it means</th><th scope="col">Right when</th><th scope="col">Wrong when</th><th scope="col">The bug it causes</th></tr></thead>
        <tbody>
          {situation.whenToUse.map((row) => <tr key={row.pattern}>
            <th scope="row"><strong>{row.pattern}</strong><small>{row.meaning}</small></th><td>{row.rightWhen}</td><td>{row.wrongWhen}</td><td>{row.bug}</td>
          </tr>)}
        </tbody>
      </table>
      <p>{situation.looksSafe}</p>
      <p className="workshop-task-pointer">{goal}</p>
      <details className="workshop-anchor-details">
        <summary>Where this has actually failed</summary>
        <ul className="workshop-anchors" aria-label="Where this has actually failed">
          {situation.anchors.map((anchor) => <li key={anchor.domain}><small>{anchor.kind} · {anchor.domain}</small><p>{anchor.text}</p><cite>{anchor.source}</cite></li>)}
        </ul>
        <p className="workshop-anchor-note">{anchorNote}</p>
      </details>
    </section>
  </>
}

export interface Progress {
  task: number
  completed: readonly number[]
  passed: boolean
}

// checked · current · ready (looks like current; pressing it advances) · locked.
export function ProgressStrip({ tasks, live, viewing, onView, onNext }: { tasks: readonly Task[]; live: Progress; viewing: number | null; onView: (index: number | null) => void; onNext: () => void }) {
  return <ol className="workshop-progress" aria-label="Question progress">{tasks.map((item, index) => {
    const kind = live.completed.includes(index) || (index === live.task && live.passed) ? 'checked'
      : index === live.task ? 'current'
        : index === live.task + 1 && live.passed ? 'ready' : 'locked'
    const status = kind === 'checked' ? (index === live.task ? (index === tasks.length - 1 ? 'Checked · lesson complete' : 'Checked · continue with step ' + (index + 2)) : 'Checked · press to read again')
      : kind === 'current' ? 'Current'
        : kind === 'ready' ? 'Ready — press to continue' : `Unlocks after question ${index}`
    const open = () => kind === 'ready' ? onNext() : onView(index === live.task ? null : index)
    return <li key={item.title}>
      <button type="button" aria-current={index === live.task ? 'step' : undefined} aria-pressed={viewing === index} className={`step-${kind}`} onClick={open}>
        <span className="step-number">{index + 1}</span><span className="step-title">{item.title}</span><small className="step-status">{status}</small>
      </button>
    </li>
  })}</ol>
}

export function LockedPreview({ id, index, tasks, liveTask, reuses, onBack }: { id: string; index: number; tasks: readonly Task[]; liveTask: number; reuses: string; onBack: () => void }) {
  const task = tasks[index]
  return <section className="workshop-preview" aria-labelledby={id}>
    <p className="kicker">Question {index + 1} of {tasks.length} · preview</p>
    <h3 id={id}>{task.title}</h3>
    <p><strong>{task.question}</strong></p>
    <p>{task.instruction}</p>
    {/* Question 2 builds its own trace; only the prediction questions replay the one from question 1. */}
    <p role="status" aria-label="Step status" className="workshop-preview-status">Locked: unlocks after question {index} is checked.{index >= 2 && ` It reuses the ${reuses} you build in question 1.`}</p>
    <button className="booking-primary" onClick={onBack}>Back to question {liveTask + 1}</button>
  </section>
}

export function QuestionHeader({ index, total, task, lead, readOnly }: { index: number; total: number; task: Task; lead: string; readOnly: boolean }) {
  return <header className="workshop-question">
    <p className="kicker">Question {index + 1} of {total}{readOnly && ' · checked · read-only'}</p>
    {readOnly && <p className="workshop-readonly-note">Question {index + 1} is checked. This view is read-only.</p>}
    <h3>{task.title}</h3><p><strong>{task.question}</strong></p><p>{task.instruction}</p>
    {lead && <p className="workshop-lead">{lead}</p>}
  </header>
}

const stripComment = (line: string) => line.replace(/\s*\/\/.*$/, '')

// Blocks of the compiled C++ with gutter notes. Notes match the raw line (comments included),
// the displayed line has its comment removed. The block containing `active` is highlighted.
export function CodeListing({ code, annotations, active, children }: { code: string; annotations: readonly { match: string; note: string }[]; active: string | null; children: ReactNode }) {
  return <section className="workshop-code" aria-label="Code for this task">
    <h4>Operations from the compiled C++</h4>
    <div className="code-listing" tabIndex={0} role="group" aria-label="Task C++ operations">{code.split('\n\n').map((block, index) => (
      <div key={index} className="code-block" data-active={active && block.includes(active) ? '' : undefined}>
        {block.split('\n').map((line, lineIndex) => {
          const note = annotations.find(({ match }) => line.includes(match))?.note
          return <Fragment key={lineIndex}><code>{stripComment(line)}</code><span className="code-note">{note ?? ''}</span></Fragment>
        })}
      </div>
    ))}</div>
    {children}
  </section>
}

export function HowTo({ guide }: { guide: Guide }) {
  return <ol className="booking-howto" aria-label={guide.howToLabel}>{guide.howTo.map((item) => <li key={item}>{item}</li>)}</ol>
}

export function GuidedStep({ first, onRun }: { first: boolean; onRun: () => void }) {
  return <div className="booking-first-step">
    <button className="booking-primary" onClick={onRun}>Run the next step for me</button>
    <span>{first ? 'Not sure where to start? This runs one step and stops; press again for the next.' : 'Runs one more step and stops.'}</span>
  </div>
}

export interface ControlsProps {
  readOnly: boolean
  liveTask: number
  onBack: () => void
  canVerify: boolean
  verdictOk: boolean
  hasVerdict: boolean
  hasNext: boolean
  hint: string | null
  canUndo: boolean
  onVerify: () => void
  onNext: () => void
  onUndo: () => void
  onResetTask: () => void
  onRestart: () => void
}

export function Controls(p: ControlsProps) {
  if (p.readOnly) return <div className="booking-replay" role="group" aria-label="Question controls">
    <button className="booking-primary" onClick={p.onBack}>Back to question {p.liveTask + 1}</button>
  </div>
  const describedBy = p.hint ? 'controls-hint' : undefined
  return <div className="booking-replay" role="group" aria-label="Question controls">
    <button className="booking-primary" disabled={!p.canVerify || p.hasVerdict} aria-describedby={describedBy} onClick={p.onVerify}>Check this task</button>
    {p.hasNext && <button disabled={!p.verdictOk} aria-describedby={describedBy} onClick={p.onNext}>Next question</button>}
    <button disabled={!p.canUndo} onClick={p.onUndo}>Previous</button>
    <button onClick={p.onResetTask}>Reset task</button>
    <button onClick={p.onRestart}>Restart lesson</button>
    {p.hint && <small id="controls-hint" className="booking-check-hint">{p.hint}</small>}
  </div>
}

const counted = ['', 'One', 'Two', 'Three', 'Four', 'Five', 'Six', 'Seven', 'Eight']

export function Takeaway({ summary, prompt, checks, total = 4 }: { summary: string; prompt: string; checks: string; total?: number }) {
  return <section className="booking-takeaway">
    <h3>{counted[total] ?? total} task checks complete</h3><p>{summary}</p>
    <details><summary>Explain the result in your own words</summary><p>{prompt}</p><p>{checks}</p></details>
  </section>
}

export function FullExample({ source, children }: { source: string; children: ReactNode }) {
  return <div className="booking-details"><details><summary>Full compiled example and limits</summary>
    {children}
    <pre tabIndex={0} aria-label="Full C++ example"><code>{source}</code></pre>
  </details></div>
}

export const RESET_TASK_NOTICE = 'This task was reset. Earlier checked tasks are unchanged.'
export const RESTART_NOTICE = 'Lesson restarted. No answers are retained.'
export const DRY_RUN_PENDING_NOTE = 'On purpose, this dry run still shows the code from before this question’s change: that change is what you are asked to predict. It switches once you check.'
export const SIMULATION_NOTE = 'Simulation: nothing runs on its own. Each button press runs one step of one thread.'
