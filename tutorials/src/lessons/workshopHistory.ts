// Shared history helpers for workshop lessons. A lesson's whole state is the list of
// events in its URL (?lesson=<slug>&v=<n>&events=A,B,verify,next,...); replaying them
// rebuilds the state, so links restore everything and "Previous" is one slice.

export const HISTORY_LIMIT = 128
// The longest event name this lesson writes into a URL, with room to spare: the two caps must
// agree, or a learner can build a history that replays fine and then fails to reload, wiping
// every checked question. A radio toggle appends one event, so this is easy to reach.
const LONGEST_EVENT_NAME = 40
export const HISTORY_CHARS = HISTORY_LIMIT * (LONGEST_EVENT_NAME + 1)

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
    if (raw.length > HISTORY_CHARS) throw new Error('History too long')
    const events = (raw ? raw.split(',') : []) as E[]
    replay(events)
    return { events, error: null }
  } catch {
    return { events: [], error: INVALID_LINK }
  }
}
