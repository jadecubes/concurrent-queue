import { beforeEach, expect, it } from 'vitest'
import { render, screen } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { PassingWorkshop } from './PassingWorkshop'
import { guided, href, type Event } from '../lessons/passingWorkshop'
beforeEach(() => window.history.replaceState(null, '', '/'))
it('shows the four needs and vocabulary before the first actor action', () => {
  render(<PassingWorkshop />)
  expect(screen.getByRole('heading', { name: 'Two threads, different speeds' })).toBeVisible()
  expect(screen.getByText(/SPSC means/)).toBeVisible()
  expect(screen.getByRole('button', { name: 'P: push(job 1)' })).toBeEnabled()
  expect(screen.getByRole('button', { name: 'Check this task' })).toBeDisabled()
})
it('guides a wait and resume, replays via URL, and preserves completed questions', async () => {
  const user = userEvent.setup()
  render(<PassingWorkshop />)
  for (let i = 0; i < 5; i++) await user.click(screen.getByRole('button', { name: 'Run the next step for me' }))
  await user.click(screen.getByRole('button', { name: 'Check this task' }))
  expect(screen.getByRole('status', { name: 'Task feedback' })).toHaveTextContent('decremented size')
  expect(window.location.search).toContain('verify')
  await user.click(screen.getByRole('button', { name: 'Next question' }))
  await user.click(screen.getByRole('button', { name: /Make room for a sleeping producer.*press to read again/ }))
  expect(screen.getByText(/This view is read-only/)).toBeVisible()
  expect(screen.queryByRole('button', { name: 'Run the next step for me' })).not.toBeInTheDocument()
})
it('keeps ownership premises visible, gives causal feedback for each misconception and permits correction', async () => {
  const events: Event[] = [...guided[0], 'verify', 'next', ...guided[1], 'verify', 'next']
  window.history.replaceState(null, '', href(events))
  const user = userEvent.setup()
  render(<PassingWorkshop />)
  expect(screen.getByRole('region', { name: 'Argument ownership contract' })).toHaveTextContent('bool try_push(T value)')
  expect(screen.getByText(/^Job j = make_job/)).toBeVisible()
  await user.click(screen.getByRole('radio', { name: /The same job:/ }))
  await user.click(screen.getByRole('button', { name: 'Check this task' }))
  expect(screen.getByRole('status', { name: 'Task feedback' })).toHaveTextContent('before checking capacity')
  await user.click(screen.getByRole('radio', { name: /A moved-from j:/ }))
  await user.click(screen.getByRole('button', { name: 'Check this task' }))
  expect(screen.getByRole('button', { name: 'Next question' })).toBeEnabled()
})
it('explains invalid links and resets safely', () => {
  window.history.replaceState(null, '', '?lesson=passing-an-item&v=0&events=next')
  render(<PassingWorkshop />)
  expect(screen.getByRole('alert')).toHaveTextContent('invalid')
  expect(screen.getByRole('button', { name: 'P: push(job 1)' })).toBeEnabled()
})
it('reset task and Previous preserve earlier checkpoint history', async () => {
  window.history.replaceState(null, '', href([...guided[0], 'verify', 'next', 'pop']))
  const user = userEvent.setup()
  render(<PassingWorkshop />)
  await user.click(screen.getByRole('button', { name: 'Previous' }))
  expect(window.location.search).toContain('verify%2Cnext')
  await user.click(screen.getByRole('button', { name: 'Reset task' }))
  expect(window.location.search).toContain('verify%2Cnext')
  expect(screen.getByRole('button', { name: 'Previous' })).toBeDisabled()
})

it('shows one consistent snapshot reveal and attributes the intervening call to P2', async () => {
  const events: Event[] = []
  for (let i = 0; i < 5; i++)events.push(...guided[i], 'verify', 'next')
  window.history.replaceState(null, '', href(events))
  const user = userEvent.setup()
  render(<PassingWorkshop />)
  await user.click(screen.getByRole('radio', { name: /Yes: the other producer/ }))
  await user.click(screen.getByRole('button', { name: 'Check this task' }))
  const stage = screen.getByRole('group', { name: 'Simulated execution' })
  expect(stage.querySelectorAll('.queue-figure')).toHaveLength(1)
  expect(stage).toHaveTextContent('P2: push(job 2)')
  expect(stage).toHaveTextContent('Accepted: 1, 2')
})
