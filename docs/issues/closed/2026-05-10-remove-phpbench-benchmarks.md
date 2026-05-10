# PHPBench系ベンチ基盤を削除する

- Status: Closed
- Created: 2026-05-10
- Closed: 2026-05-10
- Branch: feature/opentelemetry-bench
- Owner: Codex

## Background

PHPBenchベースの旧ベンチ入口はCIで使っておらず、現在の主要計測は `bench/` の専用runnerとOTEL summaryへ移行している。

## Scope

- `phpbench/phpbench` dev dependencyを削除する。
- PHPBench attribute bench、旧 `bench/run.sh` / `bench/baseline.sh` / `bench/compare*.sh`、aggregate parse / baseline compare toolを削除する。
- 現行運用docsとAGENTSの入口を `bench/` に揃える。

## Non-Goals

- 過去の計測結果docsを書き換えること。
- Phase 2計測計画docを書き換えること。
- `bench/` runnerのJSON/TSV出力を削除すること。

## Verification

- `docker compose run --rm dev sh -lc 'composer validate --strict && composer show phpbench/phpbench >/tmp/phpbench-show 2>&1; status=$?; if [ $status -eq 0 ]; then cat /tmp/phpbench-show; exit 1; fi; php tools/benchmark/contract-smoke.php --output=/tmp/contract-smoke.json'`
- `composer.json` is valid.
- `phpbench/phpbench` is no longer installed as a direct dev dependency.
- `tools/benchmark/contract-smoke.php` runs successfully.
