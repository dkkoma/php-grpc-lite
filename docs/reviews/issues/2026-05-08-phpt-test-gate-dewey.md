# PHPT test gate Dewey review 2026-05-08

## Scope

- `ext/grpc/tests/*.phpt`
- `ext/grpc/tests/helpers.inc`
- `bench/check-native-phpt.sh`
- `docs/reviews/README.md`
- `docs/reviews/templates/subagent-review-prompt.md`
- `docs/reviews/issues/`
- `AGENTS.md`
- `README.md`
- `docs/code-reading-guide.md`

## Reviewer Role

- `CI/test architecture reviewer for PHP C extension`
- `C/PHP extension domain-model reviewer`

## Review Prompt Summary

- Re-review the PHPT gate after fixes for release/development gate suitability, coverage boundaries, skip/preflight behavior, deterministic CI behavior, extension-vs-wrapper responsibilities, and review record usability.

## Issues

### REVIEW-20260508-DEWEY-PHPT-001: PHPT cleanup deletes broad source-like patterns in the test directory

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `CI/test architecture reviewer for PHP C extension`
- Finding: `check-native-phpt.sh` removes every top-level `*.php` and `*.sh` file under `ext/grpc/tests` before and after running PHPT.
- Evidence: `bench/check-native-phpt.sh`
- Expected model: PHPT artifact cleanup should remove generated run-tests artifacts without deleting potential checked-out or local helper/source files.
- Why it matters: The current repository has only `.phpt` and `helpers.inc` in that directory, so the gate works today, but future helpers or local debugging scripts placed beside PHPT files could be silently removed by a CI/development command.
- Recommended fix: Clean only artifacts derived from existing `.phpt` basenames, or move run-tests output into an isolated temporary artifact directory if supported by the chosen runner invocation.
- Fix summary: `check-native-phpt.sh` のcleanupを `*.phpt` basenameから生成されるartifactだけを削除する関数へ変更し、任意の `*.php` / `*.sh` を広く削除しないようにした。
- Fix commit: `this commit`
- Verification: `./bench/check-native-phpt.sh`
- Notes: Existing `.gitignore` patterns prevent git status pollution for generated artifacts; this finding is limited to destructive cleanup breadth.

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `1 fixed`
- Design Decision: `none`
