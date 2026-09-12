import { useEffect, useState } from 'react'
import { HISTORY_LIMIT } from '../lessons/workshopHistory'

// URL-backed lesson history shared by every workshop: the events list is the state,
// `navigate` pushes a new URL, `act` appends one event after checking it replays.
export function useWorkshopHistory<E extends string>(read: (href: string) => { events: E[]; error: string | null }, href: (events: readonly E[]) => string, replay: (events: E[]) => unknown) {
  const [location, setLocation] = useState(() => read(window.location.href))
  const [notice, setNotice] = useState('')
  const [viewing, setViewing] = useState<number | null>(null)

  useEffect(() => {
    const restore = () => {
      setLocation(read(window.location.href))
      setNotice('')
      setViewing(null)
    }
    window.addEventListener('popstate', restore)
    return () => window.removeEventListener('popstate', restore)
  }, [read])

  function navigate(events: E[], message = '') {
    window.history.pushState(null, '', href(events))
    setLocation({ events, error: null })
    setNotice(message)
    setViewing(null)
  }

  function act(event: E, message = '') {
    const events = [...location.events, event]
    if (events.length > HISTORY_LIMIT) {
      setNotice('History limit reached. Restart the lesson to continue.')
      return
    }
    replay(events)
    navigate(events, message)
  }

  return { location, notice, viewing, setViewing, navigate, act }
}
