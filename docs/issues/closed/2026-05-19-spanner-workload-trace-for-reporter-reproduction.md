---
Status: Closed
Owner: Codex
Created: 2026-05-19
Related-Issue: https://github.com/dkkoma/php-grpc-lite/issues/5
---

# Spanner実ワークロード比較用traceを整備する

## 目的

real Spanner + FPM 実ワークロードで、ext-grpc 1.58 optimized と php-grpc-lite の比較条件を相手環境でも再現・比較できるようにする。

## 背景

手元環境ではGitHub issue #5で報告された大きな差を安定再現できていない。一方、相手環境には実際に差が出る条件が存在している可能性が高い。

こちらで完全なGCP/FPM/Spanner backend条件を再現し続けるより、手元で「再現しなかった条件」と同じ観測点を整備し、相手環境で同じtraceを取ってもらうほうが原因特定に近い。

## スコープ

- Laravel Spanner bench appのaction step trace。
- php-grpc-lite拡張のRPC completion trace。
- ext-grpc / php-grpc-liteの両方で同じapp step形式のNDJSONを出力する。
- php-grpc-liteでは拡張内RPC traceも同じNDJSONへ出力する。
- trace有効化はenv opt-inにする。
- traceの読み方と相手へ渡す再現条件をdocsへ残す。

## 非スコープ

- vendor配下の直接patch。
- gRPC transport本体の性能修正。
- GitHub issueへの返信文作成。
- packet / strace / tcpdump の完全自動相関。
- ext-grpc拡張内部のRPC completion trace。

## 計画

1. `LARAVEL_SPANNER_TRACE=1` で有効になるtrace recorderを追加する。
2. `/bench` request単位でtrace id / action / pidを記録する。
3. `transaction_select2_update1_insert1` の主要stepを開始・終了で記録する。
4. `GRPC_LITE_TRACE_FILE` でphp-grpc-lite拡張のRPC完了時刻、method、status、payload bytesを記録する。
5. ext-grpc / php-grpc-liteで同じapp step traceを出せることを確認する。
6. php-grpc-liteでapp stepとRPC completionを同じNDJSONへ出せることを確認する。
7. 相手環境へ渡す比較手順を記録する。

## 進捗

- `BenchApp\Bench\SpannerTraceRecorder` を追加し、env opt-inのNDJSON traceを実装した。
- Laravel `/bench` responseに `trace_id` を返し、request単位のcontextをtraceへ付与するようにした。
- `setup` / `session_pool` / `select_1row_10col` / `dml_*_10col` / `transaction_select2_update1_insert1` の主要stepを `step.start` / `step.end` として記録するようにした。
- `GRPC_LITE_TRACE_FILE` を追加し、php-grpc-lite拡張のunary / server streaming completionを `rpc.end` として記録するようにした。
- `tools/benchmark/spanner-trace-summary.php` を追加し、NDJSONからstep elapsedとgrpc-lite RPC elapsedを集計できるようにした。
- `compose.yaml` のLaravel FPM系サービスへtrace envを追加した。
- GAX logger経由の `rpc.request` / `rpc.response` 記録クラスも追加したが、現在の `google/cloud-spanner` v1 + `Connection\Grpc` 経路では確認したsmoke runでRPC logが流れていない。現時点では共通比較の一次観測点はapp step、php-grpc-liteの詳細観測点は拡張 `rpc.end` とする。

## 使い方

trace有効化:

```bash
mkdir -p var/trace
: > var/trace/spanner-trace.ndjson

LARAVEL_SPANNER_TRACE=1 \
LARAVEL_SPANNER_TRACE_FILE=/workspace/var/trace/spanner-trace.ndjson \
GRPC_LITE_TRACE_FILE=/workspace/var/trace/spanner-trace.ndjson \
BENCH_TRANSPORT_IMPL=grpc-lite \
docker compose up -d --force-recreate fpm-lifecycle-profile nginx-laravel-profile
```

1 request smoke:

```bash
docker compose run --rm loadgen -n 1 -c 1 -disable-keepalive \
  'http://nginx-laravel-profile:8080/bench?action=transaction_select2_update1_insert1'

docker compose run --rm dev php tools/benchmark/spanner-trace-summary.php \
  var/trace/spanner-trace.ndjson
```

ext-grpc比較時は `GRPC_LITE_TRACE_FILE` による `rpc.end` は出ない。`LARAVEL_SPANNER_TRACE_FILE` のapp stepを比較対象にする。

## 検証

- `docker compose run --rm dev sh -lc 'php -l ... && cd ext/grpc && phpize && ./configure --enable-grpc && make -j$(nproc)'`
  - PHP構文チェックとC拡張ビルド成功。
- `./tools/test/check-phpt.sh`
  - 16 tests, 16 passed。
- `./tools/test/check-c-static-analysis.sh`
  - cppcheck production / bench buildとも成功。
- emulator smoke:
  - `transaction_select2_update1_insert1` 1 requestでHTTP 200。
  - `spanner-trace-summary.php` で `step.start` / `step.end` / `mixed.ids` / `rpc.end` を確認。
  - `rpc.end` に `ExecuteStreamingSql`、`Commit`、session setup系RPCが出ることを確認。

## 完了条件

- traceを無効にした通常ベンチ挙動が変わらない。
- trace有効時にaction stepとphp-grpc-lite RPC completionがNDJSONへ出る。
- 相手環境で同じコマンド・同じtrace fileを取得できる手順がdocsに残る。

## 判断ログ

- ext-grpc内部のRPC completion traceはこのリポジトリから実装できないため、共通比較はLaravel app step、php-grpc-lite詳細は拡張 `rpc.end` に分ける。
- `GRPC_LITE_TRACE_FILE` は0.0.6調査用のopt-in診断機能として扱う。未設定時はファイルI/Oも時刻取得も行わない。
- GAX logger hookは追加したが、現在のSpanner v1経路ではRPC logが流れないことをsmokeで確認したため、原因調査の一次データには使わない。

## 修正コミット

- このissueを閉じる実装コミット。
