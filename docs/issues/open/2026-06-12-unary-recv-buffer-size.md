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

- 2026-06-12: 案Bで実装完了。`h2_connection` に `recv_scratch` / `recv_scratch_len`(64KB、初回利用時に `pemalloc(persistent)`)を追加し、`h2_connection_recv_scratch()` で unary / server streaming / preflight drain の 3 経路を共有化。`destroy_h2_connection` で解放。unary の 16KB スタックバッファ、server streaming の per-call `emalloc(65536)`(struct フィールド `recv_buf` / `recv_buf_len` ごと削除)、drain の 4KB スタックバッファを置き換え。

## Verification

- PHPT 15/15、C unit、静的解析、PHPUnit 30/30 すべて PASS。
- trace I/O 件数(`trace-io-probe.sh 10 1048576`、10 RPC 合計):
  - 平文受信 `wire.socket_read`: **662 → 177**(約 66 → 17.7 回/RPC、期待どおり約 1/4)
  - TLS 受信 `wire.tls_read`: **666 → 665(変化なし)**。`SSL_read` は read_ahead 有効でも「1 回の呼び出しで最大 1 TLS record(≤16KB)」しか返さないため、アプリ側バッファを 64KB にしても呼び出し回数は減らない(issue 起票時の仮説と異なる)。raw `read(2)` syscall 自体は read_ahead が既に集約済み。
- ベンチ(before: `main-baseline-20260612` + インターリーブ再計測 / after: `recv-buffer-after-20260612`):
  - payload-unary(平文)1MB p50: 554.4 → 497.9µs(インターリーブ main 再計測 538.8µs に対しても -7.6%)、4MB: 2233.2 → 2205.9µs
  - tls-payload-unary 1MB p50: 800.1 → 847.2µs、4MB: 3248.2 → 3324.7µs(揺れ幅内、SSL_read 回数が不変のため改善なしは整合的)
  - cpu-micro: tiny_unary_0b 10.9µs cpu / 31.6µs wall(悪化なし)
  - large-streaming(main をインターリーブ計測): count_10000 8223.7 → 8401.3µs(+2%、揺れ幅内)、count_100000 89697.9 → 81006.5µs(**-9.7%**、per-call 64KB emalloc 削減の効果)

## Decision Log

- 2026-06-12: **採用**。平文 read 回数 1/4 化と server streaming の per-call 64KB emalloc 排除が実測で確認でき、TLS・小 RPC で悪化なし。期待した「TLS の SSL_read 回数削減」は OpenSSL の record 単位返却の仕様上得られないことが判明した(これは #2 unary-direct-decode の期待効果の見積もりには影響しない)。TLS 側の read 呼び出し削減をするなら SSL_read_ex のループ集約など別アプローチが必要だが、本 issue のスコープ外とする。
