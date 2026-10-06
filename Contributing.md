# Contributing to MaryGuardian

Thanks for helping! Small, focused pull requests are easiest to review.

## Getting set up
Follow *Quick start* in the [README](README.md). `make` builds the app, `make test` runs the Guardian tests.
Code is C++17; please build with `-Wall -Wextra` without new warnings.

## Ground rules
- **Privacy first.** Never commit real documents, `mg_workspace/`, databases, logs or model files.
  Use synthetic samples in issues and tests.
- **Security-sensitive code** (`Guardian`, `Safety.hpp`, `Verifier`, path handling, the HTTP server) needs a test and a
  short explanation of the threat it addresses. Default-deny stays default-deny.
- **Keep the design principle:** avoid work, do it once, do it only for what changed. Don't add per-file SQL queries or
  re-reads of unchanged files to the scan path.
- Adding a document type: edit `docTypes()` in `core/Extract.hpp`; add rules in `Extract.cpp` and a test case.
- Add `// SPDX-License-Identifier: AGPL-3.0-or-later` to new source files.

## Licensing of contributions
The project is AGPL-3.0-or-later. By opening a pull request you agree to the Contributor License Agreement
(a bot will ask you to sign it once). It keeps your copyright with you and lets the maintainer offer the code under
additional licenses in the future.

## Pull request checklist
- [ ] builds cleanly, `make test` passes
- [ ] no personal data, models or workspace files in the diff
- [ ] README / docs updated if behaviour changed