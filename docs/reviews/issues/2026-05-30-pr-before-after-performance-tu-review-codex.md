# PR before/after performance and TU review 2026-05-30

## Scope

- Branch: `c-practice-work-plan`
- Base: merge-base with `main`, `b5592d4630ed092670aea6882d9369683393d458`
- `config.m4`
- `grpc.c`
- `php_grpc.h`
- `src/**/*.c`
- `src/**/*.h`
- `docs/issues/closed/2026-05-29-c-maintainability-work-plan.md`
- Existing phase/domain review records under `docs/reviews/issues/`

## Reviewer Role

- PR-level C translation-unit, build/link, performance, and verification risk reviewer

## Review Prompt Summary

- Review the whole current PR branch against the local base branch with a before/after lens. Focus on whether the move from one large/unity-like translation unit to explicit multi-TU sources is worthwhile, whether it introduces non-LTO or hot-path cost, whether header exposure and rebuild cost are acceptable, whether bench/diagnostic code is isolated, whether `config.m4` source lists are correct, and whether benchmark/verification evidence is sufficient.
- Production code was not modified.

## Before / After / Benefits / Costs or Risks / Recommendation

### Before

- The extension lived under `ext/grpc/`; `config.m4` compiled only `main.c`.
- `main.c` directly included the production implementation files: `surface.c`, `protocol_core.c`, `status_core.c`, `transport.c`, `unary_call.c`, `server_streaming_call.c`, `bridge.c`, and bench/diagnostic `.c` files when enabled.
- `transport.c` directly included `transport_core.c`.
- C unit and fuzz tests directly included core `.c` files instead of linking testable internal helpers.
- `internal.h` was a broad private header containing PHP/Zend includes, nghttp2/OpenSSL includes, surface object layouts, transport/call state, protocol/status helpers, module globals, and franken-go backend state.
- Runtime code still contained `grpc_lite.backend` / `FrankenGrpc\*` selection state even though current project policy is one nghttp2 HTTP/2 transport.

### After

- The repository root is the PHP extension root; `config.m4` compiles an explicit production source list: `grpc.c`, `src/protocol_core.c`, `src/status_core.c`, `src/transport_core.c`, `src/surface.c`, `src/transport.c`, `src/unary_call.c`, `src/server_streaming_call.c`, and `src/wrapper_adapter.c`.
- `--enable-grpc-bench` appends only `src/diagnostic/diagnostic.c` and `src/diagnostic/bench.c`.
- Production `.c` direct includes are gone; C unit/fuzz targets now compile helper sources separately.
- C implementation and internal headers are under `src/`; bench-specific state is under `src/diagnostic/` and guarded by `PHP_GRPC_LITE_ENABLE_BENCH`.
- `internal.h` is now only a private aggregate; most code includes narrower internal headers directly.
- The franken-go backend path and backend selection config were removed from the production path.

### Benefits

- The build graph now matches the implementation structure. This is a real maintainability improvement: missing declarations, accidental cross-file coupling, and duplicate-static assumptions are more likely to be caught by the compiler/linker instead of hidden by a unity include order.
- `config.m4` makes the production vs bench source boundary explicit. The diagnostic sources are not part of the normal build unless `--enable-grpc-bench` is set.
- Pure core helpers are independently linkable and testable. This improves C unit, fuzz, static-analysis, and coverage trust compared with testing `.c` files by textual inclusion.
- Removing backend selection reduces call-path branching and removes a design conflict with the current nghttp2-only transport model.
- The largest performance-sensitive callback/data path remains colocated in `src/transport.c`; the PR does not split per-byte receive parsing, nghttp2 callbacks, send callbacks, data source reads, stream close handling, or response queueing into separate TUs.

### Costs or Risks

- Non-LTO builds lose some implicit inlining that was possible when everything was textually included into `main.c`. The main risk is not every cross-TU call; it is accidentally moving tight callback/parser loops away from their callers. Current layout mostly avoids that by keeping hot HTTP/2/gRPC frame work inside `src/transport.c`.
- `src/transport.h` is still a broad internal facade: it exposes `struct _h2_connection`, `server_streaming_call_state`, and many transport/protocol helper declarations. This is acceptable as a private transitional header, but it increases rebuild fan-out and should not be treated as a stable internal API.
- Verification evidence is strong for build/test behavior but weaker for a final whole-PR performance statement. The work-plan records a Phase 3 `spanner-shape` before/after spot check around the actual multi-TU transition, but not a final HEAD-vs-merge-base benchmark for the whole PR.

### Recommendation

- The tradeoff looks worthwhile. The PR buys clear build-graph, testability, and reviewability benefits without an obvious hot-path split mistake. I would not block merge on performance grounds.
- Treat performance evidence as "no obvious regression observed" rather than "whole PR performance equivalence proven" until a final merge-base-vs-HEAD benchmark is run under the same runner/environment.
- Keep `src/transport.c` large for now unless future benchmarks justify finer splitting. Low-risk future cleanup should target cold/setup areas first, not nghttp2 callbacks or response data parsing.

## Issues

### REVIEW-20260530-001: Final whole-PR performance evidence is only partial

- Severity: `Low`
- Status: `Open`
- Reviewer role: `PR-level C translation-unit, build/link, performance, and verification risk reviewer`
- Finding: The PR records a useful Phase 3 benchmark around the key multi-TU transition, but it does not record a final whole-PR benchmark comparing the merge-base build to current HEAD. The recorded comparison is `33500e5` vs Phase 3 working tree with `spanner-shape --calls=300 --warmup-calls=20`; later follow-up changes and the full base-to-HEAD delta are covered by build/test gates, not by a final before/after benchmark.
- Evidence: `docs/issues/closed/2026-05-29-c-maintainability-work-plan.md:337`, `docs/issues/closed/2026-05-29-c-maintainability-work-plan.md:341`, `docs/issues/closed/2026-05-29-c-maintainability-work-plan.md:342`, `docs/issues/closed/2026-05-29-c-maintainability-work-plan.md:355`, `docs/issues/closed/2026-05-29-c-maintainability-work-plan.md:624`, `docs/issues/closed/2026-05-29-c-maintainability-work-plan.md:625`
- Expected model: A whole-PR before/after performance review should compare the local base/merge-base artifact and final HEAD artifact under the same benchmark runner, tag scheme, Docker image, source-build mode, and representative workload. For this PR, `spanner-shape` is the minimum useful transport-shape check; a longer or repeated run is better than one 300-call spot check.
- Why it matters: The code review did not identify an obvious non-LTO hot-path regression, but the PR intentionally changes compile/link boundaries. Without final HEAD-vs-base data, merge reviewers should avoid claiming the whole PR has proven neutral performance impact.
- Recommended fix: Before merging or before making a durable performance claim, run a final same-environment benchmark of merge-base vs HEAD, at least `./bench/run.sh spanner-shape` with enough calls/repeats to distinguish noise from a few-microsecond p50 shift. Record run IDs, commands, p50/p99, environment, and the interpretation in the work-plan or a benchmark doc. If the result is noisy but not clearly worse, record that explicitly.
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: Review-only. I did not run Docker benchmarks in this review.
- Notes: This is Low rather than Medium because the actual hot frame/callback logic remains in one TU, `config.m4` source separation is correct, and existing tests/build gates are broad.

## Design Decisions

### REVIEW-20260530-002: Multi-TU split is an acceptable tradeoff despite possible non-LTO cost

- Severity: `Design Decision`
- Status: `Accepted`
- Reviewer role: `PR-level C translation-unit, build/link, performance, and verification risk reviewer`
- Finding: Moving from `main.c` textual inclusion to explicit source files can remove some compiler visibility in non-LTO builds, but the chosen split is coarse enough that obvious per-byte/per-frame hot paths are not fragmented.
- Evidence: `config.m4:15`, `config.m4:19`, `src/transport.h:117`, `src/transport.h:120`, `src/transport.h:122`, `src/transport.h:139`, `docs/reviews/issues/2026-05-30-tu-hotpath-split-review-codex.md`
- Expected model: Split by durable ownership boundaries first. Keep tight loops and callback graphs colocated unless there is measurement-backed reason to move them.
- Why it matters: This is the core PR tradeoff. The benefit is a maintainable build graph and testable helper boundaries; the cost is potential lost cross-TU optimization.
- Recommended fix: No code change required. Do not split `src/transport.c` further across callback/parser/send hot paths without a new issue, before/after benchmark, and a specific ownership reason.
- Fix summary: `not applicable`
- Fix commit: `not applicable`
- Verification: Review-only inspection plus existing Phase 3 and hot-path review records.
- Notes: `src/protocol_core.c`, `src/status_core.c`, and `src/transport_core.c` are reasonable separate TUs because they are small, testable helpers and not per-byte loops.

### REVIEW-20260530-003: Bench/diagnostic isolation is acceptable

- Severity: `Design Decision`
- Status: `Accepted`
- Reviewer role: `PR-level C translation-unit, build/link, performance, and verification risk reviewer`
- Finding: Bench/diagnostic implementation is compiled only for `--enable-grpc-bench`, and large bench state is guarded by `PHP_GRPC_LITE_ENABLE_BENCH`.
- Evidence: `config.m4:12`, `config.m4:16`, `config.m4:17`, `src/grpc_exchange_state.h:5`, `src/grpc_exchange_state.h:28`, `src/grpc_exchange_state.h:62`, `src/grpc_exchange_state.h:102`, `src/diagnostic/bench_call.h:6`
- Expected model: Production builds should not expose bench PHP functions, link bench implementation files, or carry diagnostic-only per-call state.
- Why it matters: Diagnostic code has heavy instrumentation state and can distort production ABI, object size, or behavior if it leaks into normal builds.
- Recommended fix: No code change required. Keep future diagnostic fields behind `PHP_GRPC_LITE_ENABLE_BENCH` and keep diagnostic sources out of the normal `PHP_GRPC_SOURCES` list.
- Fix summary: `not applicable`
- Fix commit: `not applicable`
- Verification: Review-only inspection of source lists and guards. Parent issue records normal and bench build/load checks.
- Notes: `src/common.h` still includes broad dependencies, but that affects build fan-out more than production diagnostic leakage.

### REVIEW-20260530-004: Broad `src/transport.h` is acceptable for this PR, but should not expand further

- Severity: `Design Decision`
- Status: `Accepted`
- Reviewer role: `PR-level C translation-unit, build/link, performance, and verification risk reviewer`
- Finding: `src/transport.h` exposes more than an ideal narrow boundary, including concrete connection layout and many helpers. In this PR it is still private, not installed, and keeps important transport-domain state explicit after the unity split.
- Evidence: `src/transport.h:23`, `src/transport.h:63`, `src/transport.h:78`, `src/transport.h:149`, `src/internal.h`
- Expected model: Private headers may expose concrete layout where Zend/object handlers or cross-module ownership requires it, but they should be treated as implementation boundaries, not public C API.
- Why it matters: Header breadth increases rebuild cost and the chance of accidental coupling. However, forcing a finer split now could be riskier than leaving transport lifecycle and callback state together.
- Recommended fix: No immediate code change. Future cleanup should split cold/setup or ownership-specific declarations only after a separate review; do not move hot callbacks or parser helpers just to reduce header size.
- Fix summary: `not applicable`
- Fix commit: `not applicable`
- Verification: Review-only inspection.
- Notes: The PR correctly avoids creating an `include/` public C API directory.

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `1`
- Design Decision: `3`

## Review Verification

- `git diff --check b5592d4630ed092670aea6882d9369683393d458...HEAD`: PASS
- `rg '#\s*include\s+".*\.c"|#\s*include\s+<.*\.c>' -n grpc.c src tests config.m4 tools`: no matches
- Docker tests and benchmarks were not rerun by this review. I relied on the existing recorded phase verification for build/test status and performed source/diff inspection for the PR-level tradeoff.
