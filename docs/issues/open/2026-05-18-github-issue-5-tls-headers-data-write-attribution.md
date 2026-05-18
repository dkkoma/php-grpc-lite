---
Status: Open
Owner: Codex
Created: 2026-05-18
GitHub-Issue: https://github.com/dkkoma/php-grpc-lite/issues/5
---

# GitHub issue #5: TLS write 2回がHEADERS/DATA分割かの切り分け

## 目的

php-grpc-lite 0.0.5で、1 outbound RPCが `39B write` + `large write` の2 TLS recordになり、HEADERS frameとDATA frameが別recordで送信されているという報告を検証する。

## 背景

GitHub issue #5では、real Spanner TLS gRPC single-concurrency workloadで以下の構造差が報告されている。

- ext-grpc 1.58: 1 RPCあたり `sendmsg(... iovlen=2)` でHEADERS/DATAを1 TLS application-data recordへ連結
- php-grpc-lite 0.0.5: `write(39)` と `write(1219)` の2 writeが見え、約1 RTT分遅いように見える

0.0.5では `send_pending_h2_frames()` の1回の `nghttp2_session_send()` 内でTLS write coalescingを行うため、同一 `nghttp2_session_send()` に出るHEADERS/DATAは1回の `SSL_write` へまとまる設計になっている。

## スコープ

- ローカルTLS test-serverで、Spanner寄りの大きめrequest payloadを使い、HEADERS/DATAが分割writeされるか確認する。
- 39B writeが現在RPCのHEADERSなのか、前後のHTTP/2 control frameなのかを切り分ける。
- 現行コード上のflush境界を確認する。

## 非スコープ

- ext-grpcの実装模倣。
- `sendmsg` 化そのもの。OpenSSL `SSL_write` を使う限り、syscall APIは `write` になる。
- real Spanner latency差の最終原因断定。

## 調査結果 2026-05-18

### ローカルTLS strace

warm channelで5回warmup後、stderr marker `===MEASURE===` を出してから、request payload 1800Bのunary RPCを1回実行した。

観測抜粋:

```text
read(4, ..., 16713) = 190
write(4, ..., 39) = 39
write(2, "===MEASURE===\n", 14) = 14
read(4, ..., 16713) = -1 EAGAIN
write(4, ..., 1880) = 1880
read(4, ..., 16713) = 212
write(4, ..., 39) = 39
```

解釈:

- marker後の測定対象RPC送信は `write(1880)` 1回。
- 1880B writeは、1800B request payload + gRPC 5B prefix + HTTP/2 DATA frame header + HEADERS frame等を含むサイズで、HEADERS/DATAが同一TLS writeにまとまっている。
- marker直前の `write(39)` は直前warmup RPCのresponse処理後に送られたHTTP/2制御frame。
- marker後のresponse後に出る `write(39)` も、測定対象RPC requestのHEADERSではなくresponse処理後のHTTP/2制御frame。
- marker直後の `read(...)=EAGAIN` はpersistent TLS connection reuse前のpreflight/peek相当のreadで、request body送信前に発生するが、poll waitやRTTを発生させるものではない。

### コード上のflush境界

- `send_callback()` はTLS時に `h2_connection_buffer_or_write()` へ渡し、即 `SSL_write` しない。
- `send_pending_h2_frames()` は `nghttp2_session_send()` の完了後に `h2_connection_flush_write_buffer()` を1回呼ぶ。
- 16KiB以下のcallback chunkはconnection-local bufferに集約される。
- 16KiB超のpayloadだけはcopy回避のためbuffer flush後に直接writeする。

このため、通常のsmall/medium unary requestでHEADERSとDATAが同一 `nghttp2_session_send()` 内に出る限り、HEADERS/DATAは1 TLS writeにまとまる。

## 現時点の解釈

- issue #5の `write(39)` は、測定対象RPCのHEADERSではなく、前RPCまたは同RPC response後のHTTP/2 control frameである可能性が高い。
- `first write` からresponse readまでをRPC latencyと見ると、前RPCのcontrol frameを起点にしてしまうため、約1 RTT差に見える可能性がある。
- 少なくともローカルTLS test-serverでは、0.0.5 currentの測定対象RPC requestは1 `write` にまとまっており、HEADERS/DATA分割は再現しない。
- real Spannerで同じ切り分けをするには、RPC boundary marker、またはHTTP/2 frame typeを復号できるログとstraceを対応させる必要がある。

## 次の確認候補

1. real Spanner workloadでrequest boundary markerをstderrなどに出し、marker前後の `write(39)` が前RPC由来か現在RPC由来か確認する。
2. `SSLKEYLOGFILE` + Wireshark/tshark、またはnghttp2 frame callbackの一時診断で、39B recordのHTTP/2 frame typeを特定する。
3. 39B control frameを次RPCの前ではなくresponse処理直後に必ずflushしている現状が問題になるかを確認する。ただしこれは既に現状の挙動で、request HEADERS/DATA coalescingとは別問題。
4. real Spannerでまだlatency差が残るなら、TLS record数ではなく、preflight `SSL_peek`、poll strategy、Spanner server pacing、GAX request lifecycleを別issueで切り分ける。

## 判断

- Status: Open
- 判断: ローカルではHEADERS/DATA分割は再現せず、issue #5のstrace解釈は前後のHTTP/2 control frameを現在RPC request writeに含めている可能性が高い。
- 優先度: 追加のreal Spanner marker付きtraceで確認するまでは、実装変更ではなく観測の切り分けを優先する。
