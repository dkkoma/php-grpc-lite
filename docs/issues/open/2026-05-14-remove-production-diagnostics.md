---
Status: Open
Owner: Codex
Created: 2026-05-14
Branch: perf/remove-production-diagnostics
---

# production hot path diagnostic residue cleanup

## 目的

production buildで動く不要な診断・計測残滓を削除し、wall time比較のノイズとhot path固定費を減らす。

## 背景

`--enable-grpc-bench` 無効時でも、production経路に `monotonic_us()` による細分計測、frame/call counters、診断用のready timestampなどが残っている。現在のベンチ方針は外側のOTEL spanによるwall time計測なので、内部diag値は不要になっている。

## スコープ

- production buildで不要な時間計測を `PHP_GRPC_LITE_ENABLE_BENCH` 配下へ移動または削除する。
- production buildで不要なframe/call countersを `PHP_GRPC_LITE_ENABLE_BENCH` 配下へ移動または削除する。
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

## 検証予定

- PHPT。
- C unit。
- `./bench/compare.sh spanner-shape`。
- 必要に応じて `metadata-header` / `spanner-real-client`。
