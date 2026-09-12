// Shared history helpers for workshop lessons. A lesson's whole state is the list of
// events in its URL (?lesson=<slug>&v=<n>&events=A,B,verify,next,...); replaying them
// rebuilds the state, so links restore everything and "Previous" is one slice.

export const HISTORY_LIMIT = 128

// A completed task's history: everything up to the 'next' that left it.
export function eventsThroughTask<E extends string>(events: readonly E[], task: number): E[] {
  let current = 0
  for (const [index, event] of events.entries()) {
    if (event !== 'next') continue
    if (current === task) return events.slice(0, index)
    current++
  }
  return [...events]
}

export function lessonHref(lesson: string, version: string, events: readonly string[]) {
  const query = new URLSearchParams({ lesson, v: version })
  if (events.length) query.set('events', events.join(','))
  return `?${query}`
}

export const INVALID_LINK = 'This link is invalid or uses an earlier lesson revision. The tasks have been reset; no old answer was counted.'

// The bare site root is a fresh start; any other query must name this lesson and revision
// and replay without error. Anything else is reported, never silently reinterpreted.
export function readLessonLocation<E extends string>(lesson: string, version: string, href: string, replay: (events: E[]) => unknown): { events: E[]; error: string | null } {
  try {
    const query = new URL(href).searchParams
    if (!query.size) return { events: [], error: null }
    if (query.get('lesson') !== lesson || query.get('v') !== version) throw new Error('Unknown revision')
    const raw = query.get('events') ?? ''
    if (raw.length > 1500) throw new Error('History too long')
    const events = (raw ? raw.split(',') : []) as E[]
    replay(events)
    return { events, error: null }
  } catch {
    return { events: [], error: INVALID_LINK }
  }
}

// The one hint under the controls: it must explain every disabled button and say where to go.
// `building` is the lesson's own wording for the trace-building questions (null once a trace is checkable).
export function controlsHint(args: { verdictOk: boolean; verdictFailed: boolean; canVerify: boolean; answering: boolean; hasNext: boolean; building: string | null }): string | null {
  if (args.verdictOk) return null
  const first = args.verdictFailed
    ? args.answering ? 'Not yet. Change your answer above and check again.' : 'Not yet. Press Reset task, build a different ordering, and check again.'
    : args.canVerify ? 'Press Check this task.'
      : args.answering ? 'Check this task is enabled once you select an answer above.'
        : args.building ?? ''
  return [first, args.hasNext ? 'Next question unlocks when the check passes.' : ''].filter(Boolean).join(' ')
}
