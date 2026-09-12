# Q1 implementation plan

Goal: implement the user-supplied Q1 brief and lab design sections 3–5 in an independent tutorials application. Use the existing isolated worktree. Do not commit.

1. Audit first: pin 8c68e32 and document every operation, ownership, lifetime, implementation limits and README differences in q1-source-audit.md.
2. Copy the lab's package manifest, lockfile, Vite/Playwright/TypeScript/test setup, WorkshopShell, history hook/helpers and booking.css. Keep dependency versions exact. Adapt only application identity, source serving and tutorial-specific components.
3. Test first: model tests assert full/empty waits, separate notification/resume, close and drain, by-value rvalue failure, UB termination and exhaustive item conservation. Implement passingWorkshop.ts plus checkpoint events/replay and enumerateOrderings. Reject illegal histories.
4. Build PassingWorkshop with six separately replayable checkpoints, source-derived snippets, introductory need scenes/vocabulary, live slots/wait sets/timeline and causal feedback. Add component tests for controls, read-only completed questions, reset and invalid links.
5. Add an independent standalone C++ witness and tutorials-only CMake/CTest target. Adapt run_blocked's timeout/unblock/result pattern with explicit thread join. Never execute UB. Keep existing C++ files and jobs byte-for-byte unchanged.
6. Add one tutorial CI job with pnpm/node setup, frozen install, unit tests, build, Chromium install and e2e. Add local-run docs and audit/question/evidence records.
7. Run unit tests, typecheck/build, desktop/mobile Playwright plus axe, keyboard/reduced-motion checks; inspect screenshots via a temporary e2e spec and delete it. Build/run C++ witness, check formatting/tidy compatibility and protected-tree diff. Record unexecuted learner/deployment/compiler checks explicitly.
