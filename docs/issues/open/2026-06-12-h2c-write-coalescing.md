---
Status: Open
Owner: Claude
Created: 2026-06-12
Branch: perf/h2c-write-coalescing
Related: docs/issues/closed/2026-05-15-native-h2-write-coalescing.md
---

# 平文 (h2c) 接続でも write coalescing を有効化して send() syscall とパケット数を減らす

## 目的

TLS 接続でのみ有効になっている HTTP/2 write coalescing を平文接続にも適用し、フレームごとの `send()` syscall と(TCP_NODELAY 環境での)小パケット分割を排除する。

## 背景

`send_pending_h2_frames_with_deadline` (`src/transport.c:1736`) は

```c
connection->write_coalescing = connection->tls;
```

としており、平文接続では `h2_connection_buffer_or_write` (`transport.c:1354`) が常に直接 `send()` する。nghttp2 の `send_callback` はフレーム単位(場合によりフレームヘッダと payload 別々)で呼ばれるため、平文では 1 RPC の送信が「SETTINGS」「HEADERS」「DATA」…と複数の `send()` に分かれる。`connect_tcp` で `TCP_NODELAY` を設定済み (`transport.c:1682`) なので、それぞれが即パケットとして出ていく。

- syscall 固定費: 小さい unary でも 2〜3 回の `send()`(coalescing 有効なら 1 回)。
- パケット数増: HEADERS と小 DATA が別セグメントになり、emulator/同一ホスト間ベンチでも context switch が増える。

closed issue `2026-05-15-native-h2-write-coalescing.md` で見送られたのは `nghttp2_session_mem_send2()` + スタックバッファ方式であり、現行の `write_buffer` 方式(TLS で採用済み)とは別物。TLS で採用済みの方式のフラグを平文に広げるだけなので、追加コピー戦略は既に検証済みの形。

## spec照合

- RFC 9113 (HTTP/2): フレームの TCP セグメントへの配置に制約はなく、複数フレームを 1 write にまとめるのは完全に合法。
- gRPC spec: 影響なし。
- 失敗時の取り扱い: coalesce バッファ書き込み失敗時に接続を dead 化する既存ロジック (`transport.c:1760` のコメント参照) は平文でも同様に機能する。

## 修正方法

1. `send_pending_h2_frames_with_deadline` の `connection->write_coalescing = connection->tls;` を `= true;` に変更。
2. 平文経路では `write_buffer` が `pemalloc` されることになるため、メモリ増(16KB/接続)を許容するか確認(persistent 接続前提なら無視できる)。
3. 平文 PHPT / emulator ベンチで syscall 数(`rtk proxy strace -c` 等)と p50/p99 を比較。

## 完了条件

- 平文 unary 1 回の送信 `send()` が 1 回(preface/SETTINGS 込みの初回を除く)になっている。
- 平文・TLS 両方の既存テストが通る。
- ベンチで悪化がない(改善は小さくても可)。

## 測定ベンチマーク

- 主計測: `./bench/run.sh cpu-micro` — 平文小 RPC の per-call CPU/wall(`tiny_unary_0b` / `small_unary_100b`)。send() 集約の効果は小 RPC の固定費に現れる。
- 補助: `./bench/run.sh upload-unary` — 平文大 payload 送信。`GRPC_LITE_TRACE_FILE` 有効で 1 RPC あたりの `wire.socket_write` 件数を before/after 記録。
- 回帰確認: `./bench/run.sh tls-cpu-micro`(TLS 経路に影響がないこと)、`./bench/run.sh spanner-shape`(emulator は平文なので本 issue の恩恵側でもある)。

## Progress

## Verification

## Decision Log
