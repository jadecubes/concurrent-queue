# Q1 verification record

Worktree: `tutorials/q1-passing-an-item`, implementation pinned to 8c68e32. Changes are uncommitted.

## Review pass — 2026-09-12

- Normalised TypeScript/TSX and e2e formatting to two-space indentation, spaced operators, commas and object literals, and separate statements. Compared the emitted JavaScript syntax trees before and after formatting, including literal values: unchanged. Questions, answers, feedback and test assertions are preserved; the source audit and library headers are unchanged.
- The existing tutorial CSS uses a single `minmax(0,1fr)` code column, `white-space: pre-wrap` and `overflow-wrap: anywhere`, with notes below code and an overflow-enabled listing. No CSS or header edits were made. Fresh 400 px visual verification is incomplete: no connected browser was available, and standalone Playwright Chromium failed to launch with macOS `bootstrap_check_in ... Permission denied (1100)`.
- Added `-Wconversion` to the standalone witness. Clean Debug and TSan builds both compiled with `-Wall -Wextra -Wpedantic -Wconversion` and emitted no warnings; no C++ source correction was needed.
- README now explicitly identifies the witness as optional, gives its CMake/C++20 prerequisites, and says that the C++ library needs no Node. Tutorial/witness run commands and the unmerged v1 branch status remain documented.
- Full gate attempted: `cd tutorials && pnpm test && pnpm build && pnpm test:e2e`. Unit tests: **23/23 passed in 2 files** (1.67 s). TypeScript checking and Vite build: **passed**, 41 modules transformed (Vite: 347 ms). E2e: **exit 1 before any tests executed**; the preview server could not bind `127.0.0.1:4175` (`listen EPERM: operation not permitted`). The prior e2e success below is historical, not a successful rerun of this pass.
- Standalone Debug CTest: **1/1 passed**, 0.43 s total. Standalone TSan CTest: **1/1 passed**, 1.07 s total, no sanitizer report. Both printed `Q1: full wait, close/drain, and argument ownership witnessed`.
- No commit, branch creation, staging or push was performed.

## Initial implementation verification (historical)

- Exact dependency and devDependency version comparison against cpp-concurrency-lab; lockfile, tsconfig, test setup, WorkshopShell and both history files compare byte-for-byte. booking.css retains the original file as its prefix. Config differences (relative asset/source serving and dedicated preview port) are documented in the tutorial README.
- `pnpm install --frozen-lockfile`: passed. Initial offline-only attempt found an unavailable tarball; normal frozen installation succeeded. No dependency versions changed.
- `pnpm test`: **23 passed**. Contract/replay/property tests plus component tests. Tests were run against RED stubs before implementation. Additional reviewer counterexamples reproduced grading failures before their fixes.
- `pnpm build`: TypeScript check and Vite production build passed. Implementation snippets come directly from actual library sources via raw imports.
- `pnpm test:e2e`: **4 permanent tests passed**, desktop and Pixel 7. Every question and choice distractor, invalid revisions, browser reload/back, read-only completed questions, reset, keyboard, reduced motion, safe close/finish/join/destruction and axe. No axe violations in inspected states; document width assertions passed.
- Temporary visual Playwright spec captured initial, producer-waiting, ownership, UB and terminal states on desktop/mobile in the session scratchpad (`q1-<desktop|mobile>-<state>.png`). Diagram/source closeups were also read. Fixed shrinking mobile slot text and horizontally clipped source predicates. The temporary spec was removed; screenshots are not repository artifacts.
- Standalone CMake build + CTest: **1/1 passed**, AppleClang 21, Debug. Standalone TSan build + CTest: **1/1 passed**. Neither execution uses undefined behaviour; all asynchronous users are joined before queue destruction.
- clang-format dry-run passed with local LLVM tooling (including version 19). clang-tidy passed with LLVM 20, including the root-directory fallback with no compilation database. A newer local clang-tidy required explicit macOS SDK flags; that invocation also passed. The actual CI-pinned LLVM 18 runner has not been executed locally.
- Independent review reproduced three false-positive grading paths, a contradictory snapshot reveal and a wrong resume source highlight; all fixed and re-reviewed. No remaining material findings were reported. This is source/implementation review, not human learner comprehension evidence.
- Protected-tree comparison: `include/`, `tests/`, `bench/`, top-level CMake and the original CI contents are unchanged from 8c68e32. One CI job is appended. Root README adds a tutorial entry. No commit or staging performed.

## Environment issues and incomplete checks

- Local browser installation could not create the Playwright cache lock outside the writable sandbox (`EPERM` at `~/Library/Caches/ms-playwright/__dirlock`). Browser tests ran successfully with the already installed matching Chromium. The CI install step remains explicit for a clean runner.
- The copied lab preview port 4173 became occupied by the lab during verification. A test run correctly failed against the wrong lesson. Tutorial Playwright now uses 4175 with `reuseExistingServer: false`; the corrected build passed there.
- GCC 13.2.0 could not validate this macOS SDK: first its compiler probe could not find `System`; an explicit SDK path got through configuration but failed in GCC's installed `include-fixed/stdio.h` against the newer SDK (`FILE` missing). GCC verification remains incomplete. No library or witness changes were made to work around these system-header errors.
- Hosted GitHub CI and exact clang-format/clang-tidy 18 checks have not run; the job definition is present. Existing C++ test/benchmark suites were not rerun because their files/jobs are unchanged; the standalone witness covers the new C++ artifact.
- No public deployment and no human learner session were performed. The local production preview and replay deep links were exercised. A scripted walkthrough is not a mastery claim.

## Exact new CI steps

One `tutorial` job, Ubuntu, `defaults.run.working-directory: tutorials`:

1. `actions/checkout@v4`
2. `pnpm/action-setup@v4` with pnpm `10.33.4`
3. `actions/setup-node@v4` with Node `22`, pnpm cache and `tutorials/pnpm-lock.yaml`
4. `pnpm install --frozen-lockfile`
5. `pnpm test`
6. `pnpm build`
7. `pnpm exec playwright install --with-deps chromium`
8. `pnpm test:e2e`

The standalone C++ witness is an optional separate build, not added to root CMake or the tutorial web job. Existing test-tsan, release and lint job bodies remain byte-for-byte unchanged.
