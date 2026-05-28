---
Status: Open
Owner: Codex
Created: 2026-05-28
Branch: main
---

# ZTS正式サポート

## 目的

`php-grpc-lite` のZTS PHP環境を正式サポート対象にし、source-built `ext/grpc` がZTS buildで継続的にbuild/load/testされる状態にする。

## 背景

`composer.json` の `php-ext.support-zts` は `true` だが、`docs/SPEC.md` はZTSを将来検討としていた。HTTP/2 transportのpersistent connection cacheはmodule globals上にあり、ZTSではthread-local cacheとして扱う設計にする必要がある。

現行コードは `ZEND_DECLARE_MODULE_GLOBALS(grpc_lite)` / `ZEND_MODULE_GLOBALS_ACCESSOR` を使い、`COMPILE_DL_GRPC && ZTS` では `ZEND_TSRMLS_CACHE_UPDATE()` / `ZEND_TSRMLS_CACHE_DEFINE()` も持つ。一方で、ZTS PHP上でのbuild/load/PHPTを再現可能なrunnerとCI gateがなかったため、正式サポートとしては検証線が不足していた。

## スコープ

- ZTS PHP Docker image / compose serviceを追加する。
- ZTS PHP上で `phpize && ./configure --enable-grpc && make`、extension load、PHPTを実行するrunnerを追加する。
- CIのNative QAにZTS PHPT jobを追加する。
- ZTS/NTSの代表性能比較QAを追加する。
- SPEC / native test framework / release QA checklistのZTS記述を更新する。
- C拡張内のZTSリスクを棚卸しし、必要なら追加修正する。

## 非スコープ

- PHP 8.4未満のZTS対応。
- 1つのHTTP/2 session/socketを複数threadで共有する設計。
- transport専用threadやasync event loopの導入。
- FrankenPHP grpc-go backendのZTS互換性保証。

## 計画

1. ZTS検証環境を追加する。
2. ZTS PHPT runnerを追加し、ZTS buildであることをpreflightする。
3. CIにZTS PHPT jobを追加する。
4. NTS/ZTSの代表性能比較runnerを追加する。
5. module globals、persistent connection cache、static mutable state、resource lifecycleをZTS観点でレビューする。
6. ZTS PHPTと通常PHPTをDocker内で実行する。
7. NTS/ZTS代表性能比較を実行し、OTEL summaryを記録する。
8. 必要に応じてHTTP/2/gRPCドメインモデルレビューを残し、Blocker / High / Medium / Lowがnoneになるまで修正する。

## 進捗

- 2026-05-28: ZTS正式サポートの親issueを作成。
- 2026-05-28: `Dockerfile.zts` と `dev-zts` compose serviceを追加。
- 2026-05-28: `tools/test/check-zts-phpt.sh` を追加。
- 2026-05-28: CI `Native QA` に `ZTS PHPT` jobを追加。
- 2026-05-28: SPEC / native test framework / release QA checklistのZTS gate記述を更新。
- 2026-05-28: NTS/ZTS代表性能比較用 `tools/test/check-zts-performance.sh` を追加。
- 2026-05-28: 追加レビューで、`SIGPIPE` のprocess-wide変更、runtime `getenv()` trace設定、persistent connection cacheのthread-local invariantを正式サポート前の確認タスクに追加。

## 検証

- `./tools/test/check-zts-phpt.sh`: PASS, ZTS PHP 8.4.21, PHPT 16/16
- `./tools/test/check-phpt.sh`: PASS, NTS PHP 8.4.20, PHPT 16/16
- `./tools/test/check-c-static-analysis.sh`: PASS
- `BENCH_TAG=zts-compare-smoke-20260528 ZTS_PERF_ARGS=--calls=5 ./tools/test/check-zts-performance.sh`: PASS
  - `spanner-shape` NTS/ZTS smoke run id:
    - `zts-compare-smoke-20260528-nts-spanner-shape`
    - `zts-compare-smoke-20260528-zts-spanner-shape`
  - `metadata-header` NTS/ZTS smoke run id:
    - `zts-compare-smoke-20260528-nts-metadata-header`
    - `zts-compare-smoke-20260528-zts-metadata-header`
  - `--calls=5` のrunner smokeであり、正式な性能判断値ではない。正式QAではcalls/warmup/repeatを増やして再計測し、代表値を `docs/benchmarks/` またはこのissueへ記録する。

## 判断ログ

- ZTSでのpersistent connection cacheはmodule globals上のthread-local cacheとして扱う。threadをまたいでsocket/sessionは共有しない。
- TSanはthread-safety regression検出に有用だが、ZTS build/load互換性そのものの代替にはしない。ZTS PHPTを独立gateにする。
- `Grpc\VERSION` / package versionとは別に、ZTS対応可否はPIE metadataとCI gateで管理する。
- ZTS正式サポートでは機能互換だけでなくNTSとの代表性能比較をQA evidenceに含める。初期対象は `spanner-shape` と `metadata-header` とし、必要なら `tls-spanner-shape` / `large-streaming` を追加する。

## 追加確認タスク

- `ext/grpc/main.c` の `signal(SIGPIPE, SIG_IGN)` はthreaded SAPIではprocess-wide状態変更になる。ZTS正式サポート前に、維持するなら明示的なprocess-wide policyとして文書化し、可能ならsocket/TLS write側のエラー処理で代替できるか確認する。
- `GRPC_LITE_TRACE_FILE` / `GRPC_LITE_TRACE_WIRE_BYTES` / `GRPC_LITE_TRACE_CALLS` のruntime `getenv()` はprocess-global環境に依存する。ZTSでの正式サポート前にINI/module globals化またはrequest/thread-local cache化を検討する。
- persistent connection cacheは `PHP_GRPC_LITE_G(persistent_connections)` 経由のthread-local所有を不変条件にする。`h2_connection` / `nghttp2_session` / socket / `SSL*` をthread間共有しないことをコメントまたはテストで明示する。

## 完了条件

- `./tools/test/check-zts-phpt.sh` がDocker内で通る。
- 通常NTSの `./tools/test/check-phpt.sh` が引き続き通る。
- C static analysisが通る。
- `./tools/test/check-zts-performance.sh` を実行し、NTS/ZTSの代表性能比較結果をこのissueまたは `docs/benchmarks/` に記録する。
- CI `Native QA` の `ZTS PHPT` jobが通る。
- ZTS観点のコードレビューでBlocker / High / Medium / Lowがnoneになる。
- 必要なレビュー記録と検証結果をこのissueに追記し、`Status: Closed` にして `docs/issues/closed/` へ移動する。
