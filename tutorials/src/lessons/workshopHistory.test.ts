import { describe, expect, it } from 'vitest'
import { HISTORY_CHARS, HISTORY_LIMIT, lessonHref, readLessonLocation } from './workshopHistory'

// A history the page itself will accept must survive a reload. These two caps used to
// disagree — 128 events against 1500 characters — so enough radio toggles built a state that
// replayed fine and then came back as "this link is invalid", losing every checked question.
describe('the URL and event-count caps agree', () => {
  const longest = 'lifetime-returns-first'

  it('round-trips a full-length history of the longest event name this lesson uses', () => {
    const events = Array.from({ length: HISTORY_LIMIT }, () => longest)
    const href = `https://example.test/${lessonHref('passing-an-item', '1', events)}`
    expect(readLessonLocation('passing-an-item', '1', href, () => undefined)).toEqual({ events, error: null })
  })

  it('leaves room for every event name this lesson can write', () => {
    expect(HISTORY_CHARS).toBeGreaterThanOrEqual(HISTORY_LIMIT * (longest.length + 1))
  })
})
