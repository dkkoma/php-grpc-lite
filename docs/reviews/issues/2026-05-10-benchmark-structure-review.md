# Benchmark structure hygiene review 2026-05-10

## Scope

- `bench/`
- `tools/benchmark/`
- `tools/test/` scripts that call benchmark tools
- `AGENTS.md`
- `docs/benchmarks/README.md`
- `docs/guides/opentelemetry-instrumentation.md`
- `docs/verification/release-qa-checklist.md`
- `docs/issues/closed/2026-05-10-remove-phase2-benchmark-structure.md`

## Reviewer Role

- Benchmark structure hygiene reviewer

## Review Prompt Summary

- phase2-to-current benchmark rename後の現行benchmark構造を確認する。
- Active code / scripts / docs / current guidanceに `phase2` 系の概念が残っていないか確認する。
- OTEL-only移行後にJSON/TSV result persistence、旧baseline、PHPBench、dead result/contract helpers、stale suite/script/docが現行導線に残っていないか確認する。
- Historical research docsは、現行guidanceとしてリンク・提示されていない限りblocker扱いしない。

## Issues

### REVIEW-20260510-BENCH-STRUCTURE-001: Lifecycle test artifacts still use benchmark-result naming and directory

- Severity: `Low`
- Status: `Closed`
- Reviewer role: `Benchmark structure hygiene reviewer`
- Finding: Active lifecycle test scripts still write non-OTEL artifacts under the old benchmark-result surface. `check-native-fpm-lifecycle.sh` defaults to `BENCH_OUTPUT_DIR=var/bench-results` and writes `benchmark-native-fpm-lifecycle-*.json`; `check-native-lifecycle-stress.sh` writes Valgrind logs under `var/bench-results`.
- Evidence: `tools/test/check-native-fpm-lifecycle.sh`, `tools/test/check-native-lifecycle-stress.sh`
- Expected model: Current benchmark result persistence is OTEL-only via `bench/` runners and `tools/benchmark/otelop-summary.php`. Lifecycle QA artifacts should either be print-only or use test-artifact naming and directories, not benchmark-result JSON naming.
- Why it matters: Current guidance says JSON/TSV benchmark result persistence and old baseline flow are retired. Keeping active JSON output named `benchmark-*` under `var/bench-results` can make users treat lifecycle QA evidence as a supported current benchmark result format.
- Recommended fix: Move these lifecycle artifacts to a neutral test artifact location such as `var/test-results/`, rename `BENCH_OUTPUT_DIR` to a test-specific variable, and drop the `benchmark-*.json` prefix. If the FPM lifecycle JSON is intentionally retained as QA evidence, document it as non-benchmark test output.
- Fix summary: `tools/test` lifecycle artifactsを`var/test-results`へ移し、FPM lifecycle JSONをtest artifactとして命名した。Valgrind logもtest artifact配下へ移した。
- Fix commit: `8819d2e`
- Verification: `bash -n tools/test/check-native-fpm-lifecycle.sh tools/test/check-native-lifecycle-stress.sh`; targeted `rg` for active old benchmark result persistence markers.
- Notes: This does not affect the main `bench/run.sh` / `bench/compare*.sh` OTEL-only flow.

### REVIEW-20260510-BENCH-STRUCTURE-002: Current OTEL doc still describes JSON/TSV as existing benchmark boundary

- Severity: `Low`
- Status: `Closed`
- Reviewer role: `Benchmark structure hygiene reviewer`
- Finding: The active OpenTelemetry instrumentation doc says the OTEL span duration can be used at the same boundary as the "existing JSON/TSV wall time", even though current guidance says JSON/TSV benchmark result persistence is retired.
- Evidence: `docs/guides/opentelemetry-instrumentation.md`
- Expected model: Active docs should describe OTEL as the only current benchmark result source and refer to JSON/TSV only as removed legacy context when necessary.
- Why it matters: The sentence is not a runnable obsolete command, but it weakens the current guidance by implying JSON/TSV output is still an existing benchmark result surface.
- Recommended fix: Reword the sentence to say the OTEL span duration uses the same former PHP-runner wall-clock boundary as the retired JSON/TSV output, or remove the JSON/TSV comparison entirely.
- Fix summary: `docs/guides/opentelemetry-instrumentation.md`のJSON/TSV境界表現を削除し、現行のPHP runner外側wall-clock境界をOTEL span durationとして記録する説明にした。
- Fix commit: `8819d2e`
- Verification: targeted `rg` for active JSON/TSV benchmark surface wording.
- Notes: `AGENTS.md` and `docs/benchmarks/README.md` correctly state that JSON/TSV result persistence and old baseline operation are retired.

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `2 closed`
- Design Decision: `none`

## Checked

- `git status --short` was clean before review.
- Recent log includes `7d6572d Bench: phase2名のベンチ構造を現行runnerへ整理` at `HEAD`.
- Active benchmark runner paths are now `bench/run.sh`, `bench/compare.sh`, `bench/compare-spanner-dml-unary-shape.sh`, and `bench/compare-small-select-streaming.sh`.
- Active benchmark tool namespace is `PhpGrpcLite\Tools\Benchmark`; no active `PhpGrpcLite\Tools\Phase2` namespace was found.
- Active `bench/`, `tools/benchmark/`, `tools/test/`, `AGENTS.md`, `docs/benchmarks/README.md`, `docs/guides/opentelemetry-instrumentation.md`, and `docs/verification/release-qa-checklist.md` did not expose phase-based runner names or commands.
- `phase2` references in `docs/issues/closed/2026-05-10-remove-phase2-benchmark-structure.md` are historical issue context for the rename and were not treated as findings.

## Re-review 2026-05-10

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Previous Low findings: `REVIEW-20260510-BENCH-STRUCTURE-001` and `REVIEW-20260510-BENCH-STRUCTURE-002` are closed.
- Checked branch/worktree: `feature/opentelemetry-bench` with clean worktree before re-review.
- Checked phase naming: scoped `rg` found no active `phase2`, `Phase 2`, `Phase2`, `PHASE2`, `PhpGrpcLite\Tools\Phase2`, or phase2 path/namespace references outside this historical review issue.
- Checked current benchmark entrypoints: `bench/run.sh`, `bench/compare.sh`, `bench/compare-spanner-dml-unary-shape.sh`, and `bench/compare-small-select-streaming.sh` expose current `tools/benchmark/` paths and OTEL summary flow.
- Checked old result infrastructure: no active `var/bench-results`, `BENCH_OUTPUT_DIR`, benchmark JSON/TSV persistence, PHPBench, old baseline runner, ResultContract/contract smoke, aggregate parse, or dead benchmark result helper remained exposed as current.
- Checked lifecycle/stress artifacts: FPM lifecycle JSON and lifecycle Valgrind log now use `var/test-results`, `TEST_OUTPUT_DIR`, neutral `native-*` names, and `Test artifact` / `Valgrind log` wording rather than benchmark result persistence.
- Checked active docs/guidance: `AGENTS.md`, `docs/benchmarks/README.md`, `docs/guides/opentelemetry-instrumentation.md`, and `docs/verification/release-qa-checklist.md` present OTEL span / `otelop-summary.php` as the current benchmark result source and keep JSON/TSV/baseline only as retired legacy guidance.
