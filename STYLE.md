# Style guide

This project follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
for formatting and naming, enforced by `.clang-format` and `.clang-tidy` (both
checked in CI).

## Comments

- **Public API** (everything under `include/cq/`): Doxygen `///` comments on
  every public class and member. Use `@tparam`, `@param` / `@param[out]`,
  `@return`, and `@throws` tags. State the contract: blocking behavior,
  error/close semantics, ownership, and thread-safety.
- **Internal code** (tests, benchmarks, private members, function bodies):
  plain `//` prose. Explain *why*, not *what*.
- Tag hygiene is compiler-enforced: clang builds compile with
  `-Wdocumentation -Werror=documentation`, which rejects `@param` names that
  do not match the signature. Keep comments in sync with code or the build fails.
- `TODO(username): description` for known follow-ups.

## Layout

- Headers (`.hpp`) declare; template member definitions live in a matching
  `.ipp` included at the bottom of the header. No function bodies in class
  definitions in the public headers.
- Each `.ipp` includes its own `.hpp` at the top (Boost.Asio's `impl/*.ipp`
  convention) so it parses standalone in editors; the include guards on both
  files collapse the cycle, and the resulting `misc-header-include-cycle` is
  silenced with a `NOLINT` on that one line.
- Special member functions (constructors, copy/move operations, destructor)
  stay grouped at the top of the `public:` section, with a comment explaining
  any deleted operations.

## Tooling

- Format: `clang-format -i` (settings live in `.clang-format`). CI rejects
  unformatted code; format-on-save settings are checked in
  (`.vscode/settings.json`). CI pins clang-format/clang-tidy **18** — use the
  same major version locally or formatting may not match.
- Lint: `clang-tidy -p <build-dir>` (settings live in `.clang-tidy`; every
  build dir exports `compile_commands.json`).
