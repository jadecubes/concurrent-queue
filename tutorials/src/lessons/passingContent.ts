export const tasks = [
  {
    title: 'Make room for a sleeping producer',
    goal: 'Goal: P waits with job 3, C frees a slot, then P completes that same push.',
    question: 'The two slots are full. What lets Producer P’s third push continue?',
    instruction: 'Build a trace: P pushes jobs 1 and 2, then calls push(3). Let C pop once, then schedule P’s pending call to resume. Watch the freed slot.'
  },
  {
    title: 'Give a sleeping consumer an item',
    goal: 'Goal: C waits on an empty queue, P pushes, then C completes its pending pop.',
    question: 'Consumer C arrived before any work. Which step wakes it, and when does it actually get job 1?',
    instruction: 'Start with C’s pop on the empty, open queue. Then place a job with P and resume C. A notification alone does not complete pop.'
  },
  {
    title: 'Retry a value that was already moved',
    goal: 'Goal: distinguish the caller’s object from the by-value parameter.',
    question: 'The first try_push fails because the queue is full. When a slot frees up and this loop succeeds, what does it enqueue?',
    instruction: 'Job j owns a payload; its move constructor transfers that payload and marks j moved-from. No exceptions occur, nobody closes the queue, and C eventually frees a slot. Read the signature and contract beside the loop, then select an outcome.'
  },
  {
    title: 'Close, refuse, and drain',
    goal: 'Goal: closed and drained; jobs 1 and 2 delivered, job 3 rejected, and pop returned false.',
    question: 'There are two accepted jobs. Can shutdown refuse new work without losing those jobs?',
    instruction: 'Starting with jobs 1 and 2, order close, P’s push(3), and C’s pops. You may drain before or after close; P’s job 3 must be refused and a final pop must observe closed and drained.'
  },
  {
    title: 'Transfer: wake is not completion',
    goal: 'Goal: identify the missing lifetime ordering before destruction.',
    question: 'C is asleep in pop(). O calls close(), then destroys the queue before C resumes. Is that destruction defined?',
    instruction: 'This time the owner can run again before the notified consumer reacquires the mutex. P has finished. Use the visible lifetime rule and pending call, then select the explanation.'
  },
  {
    title: 'A snapshot is not a reservation',
    goal: 'Goal: explain why two individually locked calls do not reserve capacity.',
    question: 'P sees size() == 1 and capacity() == 2. Another producer fills the last slot before P calls push(x). Can P still block?',
    instruction: 'This changed scene has two producers, which MutexQueue supports. size() releases the mutex before returning; capacity is fixed. Read the source, then reason about the interval between P’s calls.'
  },
] as const

export const guides = tasks.map((_, i) => ({
  lead: [
    'A fast sender needs somewhere to leave work and a limit when the receiver falls behind.',
    'An idle receiver should sleep instead of repeatedly asking an empty queue for work.',
    'Waiting policy and argument ownership are separate decisions. A bool cannot give a rejected payload back.',
    'Shutdown must tell sleeping receivers that no more work is coming, while preserving accepted work.',
    'Returning from close is an event in O. Returning from pop is a different event in C.',
    'The queue serializes each call; your two-call decision has a gap another producer may enter.',
  ][i],
  howToLabel: (i === 0 || i === 1 || i === 3) ? 'How to play' as const : 'How to answer' as const,
  howTo: (i === 0 || i === 1 || i === 3)
    ? ['Choose a real operation on an actor card; each press advances one call until it returns or waits.', 'A sleeping actor releases the mutex. After notification, schedule its Resume button to reacquire and finish the pending call.', 'Check the task when the goal is met. Previous undoes one event; Reset task keeps earlier checked questions.']
    : ['Read the initial state and the short code contract beside this question.', 'Choose an outcome and its cause. The reference picture stays fixed while you reason.', 'Press Check this task to reveal the changed state and causal feedback; change an incorrect answer to try again.'],
}))

export const choices = {
  'ownership-same': {
    text: 'The same job: try_push leaves its argument untouched when it returns false.',
    feedback: 'P constructed the by-value parameter before checking capacity. That moved the payload out of j, even though the parameter was then discarded. False reports rejection, not preservation of j.'
  },
  'ownership-husk': {
    correct: true,
    text: 'A moved-from j: the failed call already moved its payload into a discarded parameter.',
    feedback: 'P’s first failed try_push consumed the rvalue before it inspected size. The bool result returns no payload. After C pops, retrying the same j enqueues its moved-from state. Re-materialise every pass with try_push(make_job()), or use blocking push to wait for capacity. Blocking push can still discard the parameter on close.'
  },
  'ownership-throws': {
    text: 'Nothing: try_push throws when the queue is full.',
    feedback: 'For this nonthrowing Job, full is an ordinary false return in P’s try_push. The queue does not throw for capacity exhaustion; the parameter has already been constructed and is discarded.'
  },
  'ownership-copy': {
    text: 'The job: std::move only marks j, so the queue copies it.',
    feedback: 'std::move is a cast, but that cast selects Job’s move constructor for P’s by-value parameter. Here the constructor really transfers the payload; it is not a copy.'
  },
  'lifetime-joined': {
    text: 'Yes: close() joined the consumer.',
    feedback: 'O’s close only changes closed_ and notifies both wait sets. It contains no thread handle and no join. C still has an active pop using the queue.'
  },
  'lifetime-returns-first': {
    text: 'Yes: close ran first, so C’s pop returns false before O destroys.',
    feedback: 'Close first guarantees notification, not C running next. O can destroy before C reacquires mutex_ and reads size_. In this stated schedule C has not resumed, so the lifetime rule is violated.'
  },
  'lifetime-unsafe': {
    correct: true,
    text: 'No: C’s pending pop still uses the queue. O must let C finish and join it before destruction.',
    feedback: 'O notified C, but C has not reacquired the mutex or returned from pop. Destroying now is undefined behaviour; the simulation stops here. Repair: close, let C resume and return false, finish every user, join them, then destroy. close neither joins nor forces that ordering.'
  },
  'lifetime-twice': {
    text: 'No: close() must be called twice before destruction.',
    feedback: 'A second close changes nothing: O’s close is idempotent. The missing event is C’s completion and the owner joining users, not another notification.'
  },
  'snapshot-reserved': {
    text: 'No: size() reserved the free slot for P.',
    feedback: 'P’s size() only reads size_ under its own lock. It neither changes tail_ nor reserves a slot. After it unlocks, the other producer can fill that slot.'
  },
  'snapshot-blocks': {
    correct: true,
    text: 'Yes: the other producer fills the slot after size() unlocks, so P’s push sees a full queue and waits.',
    feedback: 'P’s size() returned 1 and released mutex_. The other producer then incremented size_ to 2. P’s later push acquires the mutex afresh, sees size == capacity, and waits in not_full_. size and closed are advisory snapshots, not a check-and-act transaction.'
  },
  'snapshot-rejects': {
    text: 'No: push returns false whenever an earlier size() check becomes stale.',
    feedback: 'P’s blocking push has no memory of that size() call. It waits for capacity and returns false only on close. try_push would return false on full, but this code uses push.'
  },
  'snapshot-race': {
    text: 'This is a data race: MutexQueue cannot support two producers.',
    feedback: 'MutexQueue supports multiple producers. Each access is protected by mutex_; the problem is a stale decision across two calls, not an unprotected memory access.'
  },
} as const
export type Choice = keyof typeof choices
export const taskChoices: Record<number, readonly Choice[]> = {
  2: ['ownership-same', 'ownership-husk', 'ownership-throws', 'ownership-copy'],
  4: ['lifetime-joined', 'lifetime-returns-first', 'lifetime-unsafe', 'lifetime-twice'],
  5: ['snapshot-reserved', 'snapshot-blocks', 'snapshot-rejects', 'snapshot-race'],
}
// Each question's winning answer is the entry above that marks itself correct, so the id cannot
// be listed here and be absent from the question's own choice list.
export const correct: Record<number, Choice> = Object.fromEntries(
  Object.entries(taskChoices).map(([task, ids]) => [Number(task), ids.find(id => 'correct' in choices[id])!]),
) as Record<number, Choice>
export const annotations = [
  { match: 'std::unique_lock lock', note: 'locks shared state; wait can release this lock' },
  { match: 'std::lock_guard lock', note: 'lock held only inside this scope' },
  { match: 'not_full_.wait', note: 'P sleeps if full and open; wake reacquires and rechecks' },
  { match: 'not_empty_.wait', note: 'C sleeps if empty and open; wake reacquires and rechecks' },
  { match: 'enqueue_locked(std::move(value))', note: 'write slot, advance tail, increment size' },
  { match: 'dequeue_locked(out)', note: 'read slot, advance head, decrement size' },
  { match: 'not_empty_.notify', note: 'wake consumer(s); no mutex ownership transfer' },
  { match: 'not_full_.notify', note: 'wake producer(s); no call completes here' },
  { match: 'std::exchange(closed_', note: 'one-way switch; a second close does nothing' },
]
