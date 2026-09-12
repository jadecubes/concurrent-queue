# Passing an item — Q1

A self-contained, six-question web tutorial for `cq::MutexQueue<Job>` at capacity 2. The implementation is pinned to **8c68e32**, on the **unmerged feat/mutex-queue branch**; v1 is not on `main` yet. This is a browser simulation of the source contract, not C++ execution.

Node and pnpm are needed only for the web tutorial. The C++ library does not require Node.

## Run the tutorial

From the repository root:

```sh
cd tutorials
pnpm install --frozen-lockfile
pnpm dev --host 127.0.0.1
```

Open [the local lesson](http://127.0.0.1:5173/?lesson=passing-an-item&v=1). A reproducible full/wait/resume checkpoint is [this local deep link](http://127.0.0.1:5173/?lesson=passing-an-item&v=1&events=push,push,push,pop,resume-P,verify). The URL includes decisions and checks; reload, browser history, Previous, reset and read-only completed questions all replay those events. Invalid revisions and histories explain the reset and count no old answers.

Node 22 and pnpm 10.33.4 match the new tutorial CI job. Dependencies are pinned to the exact versions in cpp-concurrency-lab, including React 18.3.1, TypeScript 5.7.3, Vite 5.4.21, vitest 2.1.9, Playwright 1.63.0 and axe-core 4.10.2. The manifest, lockfile, configs and test setup were copied from that lab; the package name changed. Vite additionally permits direct raw imports from the parent `include/` and uses relative build asset URLs. Playwright uses **4175**, with server reuse disabled, to avoid accidentally testing the lab on its default 4173. The copied WorkshopShell, history hook/helpers and base booking.css remain local files; there is no shared framework package.

## Web checks

Run inside `tutorials/`:

```sh
pnpm test
pnpm build
pnpm exec playwright install --with-deps chromium
pnpm test:e2e
```

Playwright starts the production preview at port 4175 and checks desktop and Pixel 7 layouts, every question and distractor, axe, keyboard, reduced motion, replay and safe lifetime repair. It refuses to reuse a server already listening on that port. `pnpm build` includes TypeScript checking. Build output is `tutorials/dist/`; it can be served statically with no backend. No public deployment is claimed.

Displayed implementation and API contracts are `?raw` imports directly from `../include/cq/mutex_queue.ipp` and `.hpp`, sliced in `src/lessons/passingSource.ts`. There is no second copy of the library. The shown candidate retry/check-then-act call sites are question premises, not replacement implementations.

## What proves the C++ claims

Every contract this lesson teaches is already asserted by the library's own GoogleTest suite,
which CI runs under ThreadSanitizer on Linux and macOS:

| Claim the lesson makes | Test that witnesses it |
|---|---|
| A full `push` waits, and resumes when a `pop` frees a slot | `PushBlocksUntilPopWhenFull`, `PopsInFifoOrder` |
| An empty `pop` waits until a `push` arrives | `PopBlocksUntilPush` |
| `close` refuses producers and lets consumers drain | `PushAfterCloseFails`, `PopDrainsRemainingItemsAfterClose` |
| `close` wakes both kinds of waiter | `CloseWakesBlockedPop`, `CloseWakesBlockedPush` |
| A failed `try_push` consumes an rvalue and leaves an lvalue intact | `FailedPushConsumesRvaluesAndLeavesLvaluesIntact` |

The tutorial adds no second C++ build. An earlier draft carried a standalone witness that
re-asserted those same five facts against the same header; it was removed rather than kept in
step by hand. The library headers, tests, benchmarks, root CMake and the existing C++ CI jobs
are untouched, and nothing outside `tutorials/` depends on this directory.

## Source and learning evidence

- [Source audit and operation table](docs/q1-source-audit.md)
- [Question audit, valid alternatives and transfer rubric](docs/q1-question-audit.md)
- [Verification record and limits](docs/q1-verification.md)

Model items carry unique identities; accepted items reconcile with occupied slots, deliveries and safe destruction of undrained storage. Rejected parameters are tracked separately. Wait entry releases the mutex atomically; notification and resumption are separate events. Close wakes both wait sets but cannot join users. The model stops at undefined behaviour.

The dry run enumerates every legal order in each explicitly fixed small space: a full queue's third push versus one pop, an empty pop versus push, new calls versus close, and pending calls with close and resumption. It does not enumerate all intra-call C++ interleavings, spurious wakes or throwing payload operations. Job moves are nonthrowing in the scene. No abort, lock-free progress, fairness or completed-work guarantee is invented.

Automated task checks and an agent source review are not evidence that a human learner understands the mechanism. The final explanation prompt is for learner review; its reasoning is not graded or uploaded.
