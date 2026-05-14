---
Status: Closed
Owner: Codex
Created: 2026-05-14
Branch: perf/remove-production-diagnostics
---

# production hot path diagnostic residue cleanup

## 目的

production buildで動く不要な診断・計測残滓を削除し、wall time比較のノイズとhot path固定費を減らす。

## 背景

`--enable-grpc-bench` 無効時でも、production経路に `monotonic_us()` による細分計測、frame/call counters、診断用のready timestampなどが残っている。現在のベンチ方針は外側のOTEL spanによるwall time計測なので、production hot pathで内部diag値を集める必要はない。

## スコープ

- production経路で常時実行されている不要な時間計測を削除する。
- production経路で常時更新されているframe/call countersを削除する。
- bench専用entrypointと `bench.c` は残す。
- protocol制御、deadline、error classification、lifecycleに必要な状態は残す。
- PHPT / C unit /主要ベンチで挙動と性能を確認する。

## 非スコープ

- `bench.c` や `--enable-grpc-bench` 自体の削除。
- OTELベンチ基盤の変更。
- protocol semanticsの変更。

## 削除候補

- unary/server streamingの `setup_us` / `submit_us` / `initial_send_us` / `recv_loop_us` / `start_unix_nanos`。
- `bytes_sent` / `bytes_received`。
- `data_read_calls` / `data_recv_calls`。
- `sent_frames` / `recv_frames` / `not_sent_frames` / `last_*_frame_*`。
- observer無効時の response ready timestamp。

## 残すもの

- deadline計算・timeout判定用の `monotonic_us()`。
- `stream_error_code` / `stream_reset_seen` / `stream_refused_seen` / GOAWAY lifecycle。
- `last_io_errno` / `last_ssl_error` / error detail。
- response queue count/bytesなどflow-controlとmemory upper boundに必要な状態。

## 実施内容

- unary production経路から `setup_us` / `submit_us` / `initial_send_us` / `recv_loop_us` / `start_unix_nanos` の計測を削除。
- server streaming production経路から同様の細分時間計測と `bytes_received` / delivered payload byte計測を削除。
- transport production callbackから `data_read_calls` / `data_recv_calls` / frame counters を削除。
- 計測だけに使っていた `on_frame_send_callback` / `on_frame_not_send_callback` の登録と実装を削除。
- server streaming queue投入時に常時取っていた ready timestamp を削除。
- 追加整理として、production transportに散在していた payload copy / message ready / delivery observer hookを削除。
- bench専用fieldは `PHP_GRPC_LITE_ENABLE_BENCH` 配下に閉じ、production buildの `grpc_call` / server streaming stateから未使用fieldを削除。
- `diagnostic.c` はbench build時のみincludeし、production buildから診断record helperを外した。

## 検証

- `./tools/test/check-phpt.sh`: PASS。
- `./tools/test/check-c-unit.sh`: PASS。
- `./tools/test/check-c-static-analysis.sh`: PASS。
- 追加整理後の `./tools/test/check-phpt.sh`: PASS。
- 追加整理後の `./tools/test/check-c-unit.sh`: PASS。
- 追加整理後の `./tools/test/check-c-static-analysis.sh`: PASS。

## 完了条件

- production hot pathで常時実行されていた診断用時間計測とカウンタ更新を削除した。
- bench専用entrypointと `bench.c` は残した。
- protocol制御、deadline、error classification、lifecycle用状態は変更していない。
