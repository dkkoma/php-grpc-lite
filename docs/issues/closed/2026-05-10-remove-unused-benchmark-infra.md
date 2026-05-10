# 未使用の旧探索ベンチ基盤を削除する

- Status: Closed
- Created: 2026-05-10
- Closed: 2026-05-10
- Branch: feature/opentelemetry-bench
- Owner: Codex

## Background

PHPBench系の旧ベンチ基盤削除後も、libcurl時代やnghttp2 PoC時代の探索専用スクリプトが残っている。現在の主要ベンチは `bench/phase2/run.sh`、`bench/phase2/compare.sh`、Spanner shape比較、OTEL summaryで足りており、旧探索入口はCIや現行presetから使っていない。

## Scope

- 現行runner / preset / QAから到達しない旧libcurl・PoC・単発探索ベンチ入口を削除する。
- 対応する一時instrumentation patchを削除する。
- `run.sh` から実行できる本体が残っているだけの単発wrapperも削除する。
- 現行の `bench/phase2/run.sh` suites、Spanner代表比較、QA scriptsは残す。

## Non-Goals

- 過去の研究・計測結果docsを書き換えること。
- 現行diagnostic suiteを全面削除すること。
- `tools/phase2/ResultContract` や JSON/TSV 出力を削除すること。

## Verification

- `docker compose run --rm dev sh -lc 'for f in tools/phase2/*.php; do php -l "$f" || exit 1; done; php tools/phase2/contract-smoke.php --output=/tmp/contract-smoke.json'`
- `./bench/phase2/run.sh contract-smoke`
- Removed file names no longer appear in current AGENTS / bench / tools / benchmark README / issues / CI references.
