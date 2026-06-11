---
Status: Open
Owner: Claude
Created: 2026-06-12
Branch: perf/unary-recv-buffer-size
---

# unary 受信バッファ 16KB を server streaming と同じ 64KB に揃えて syscall 回数を減らす

## 目的

unary call の受信ループが使う 16KB スタックバッファを拡大し、大きいレスポンス受信時の `SSL_read`/`recv` syscall 回数と nghttp2 への投入回数を削減する。

## 背景

- unary: `char recv_buf[16384]` のスタックバッファ (`src/unary_call.c:64`)。
- server streaming: `state->recv_buf_len = 65536` の heap バッファ (`src/server_streaming_call.c:85`)。

INITIAL_WINDOW_SIZE はデフォルト 8MB (`grpc.c:15`、`grpc_lite.http2_stream_window_size`)、TLS では `SSL_set_read_ahead(ssl, 1)` (`src/transport.c:1133`) も有効なので、サーバは大きな塊で送ってくる。1MB のレスポンスで unary は最低 64 回の read ループ(syscall + `nghttp2_session_mem_recv` + want_write チェック)を回るが、64KB なら 16 回で済む。read_ahead 有効時は SSL 内部バッファに溜まったデータを 16KB ずつ刻んで取り出す形になり、純粋な固定費。

## spec照合

HTTP/2 / gRPC spec とは無関係(バッファサイズはローカル実装詳細)。フロー制御ウィンドウ(8MB)とは独立で、受信処理の粒度のみ変わる。

## 修正方法

案A(最小): `recv_buf` を 65536 byte に拡大。ただし 64KB のスタック確保は FrankenPHP worker / ZTS スレッドのスタックサイズによっては好ましくないため、案Bを推奨。

案B(推奨): `h2_connection` に再利用可能な受信スクラッチバッファを持たせる。

```c
/* transport.h: h2_connection に追加 */
uint8_t *recv_scratch;      /* pemalloc(65536, persistent), 初回 recv 時に確保 */
size_t   recv_scratch_len;
```

- unary / server streaming / preflight drain (`drain_pending_connection_data_for_reuse` の 4KB バッファ含む) で共有。
- 接続は同時に 1 つの受信ループしか回さない設計(`current_read_call` 単一)なので共有しても競合しない。
- `destroy_h2_connection` で解放。persistent 接続なら確保も 1 回で済み、server streaming の per-call `emalloc(65536)` (`server_streaming_call.c:86`) も削減できる。

## 完了条件

- unary の大レスポンス受信で read ループ回数が約 1/4 になっている(trace の `wire.tls_read` 件数等で確認)。
- server streaming の per-call 64KB `emalloc` が消えている(案B採用時)。
- 既存テストがすべて通る。

## 測定ベンチマーク

- 主計測: `./bench/run.sh tls-payload-unary` — TLS では read 回数削減が SSL_read 固定費削減に直結する。`payload_unary_1048576b` / `4194304b` の `wall_time_ns_per_call` を before/after 比較。
- 補助: `./bench/run.sh payload-unary`(平文)、`GRPC_LITE_TRACE_FILE` 有効で 1 RPC あたりの `wire.tls_read` 件数を before/after 記録(1MB で 約64回 → 約16回 が期待値)。
- 回帰確認: `./bench/run.sh cpu-micro`(小 RPC の固定費悪化なし)、`./bench/run.sh large-streaming`(streaming のバッファ共有化の影響確認、案B採用時)。

## Progress

## Verification

## Decision Log
