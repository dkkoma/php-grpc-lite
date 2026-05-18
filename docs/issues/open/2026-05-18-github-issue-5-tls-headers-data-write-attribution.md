---
Status: Open
Owner: Codex
Created: 2026-05-18
GitHub-Issue: https://github.com/dkkoma/php-grpc-lite/issues/5
---

# GitHub issue #5: TLS write 2回とscatter/gather送信差の切り分け

## 目的

php-grpc-lite 0.0.5で、1 outbound RPCが `39B write` + `large write` の2 TLS recordになり、HEADERS frameとDATA frameが別recordで送信されているという報告を検証する。

特に、公式ext-grpcが `sendmsg(... msg_iovlen=2)` でHEADERS bufferとDATA bufferをscatter/gather I/Oとして1 syscallに渡している点を、単なるsyscall数ではなく「複数HTTP/2 frame bufferをTLS/socket層へどう束ねるか」というtransport設計差として調べる。

## 背景

GitHub issue #5では、real Spanner TLS gRPC single-concurrency workloadで以下の構造差が報告されている。

- ext-grpc 1.58: 1 RPCあたり `sendmsg(... iovlen=2)` でHEADERS/DATAを1 TLS application-data recordへ連結
- php-grpc-lite 0.0.5: `write(39)` と `write(1219)` の2 writeが見え、約1 RTT分遅いように見える

ここでの `sendmsg(... msg_iovlen=2)` はscatter/gather、またはvectored I/Oの観測である。ext-grpc側は少なくともsocket syscall境界ではHEADERS相当bufferとDATA相当bufferをcopyせずに1回の送信操作へ渡している。一方、php-grpc-liteはOpenSSL `SSL_write` を使うため、API上は `sendmsg` ではなく `write` として見える。したがって調査では、以下を分ける必要がある。

- syscall API差: `sendmsg` / iovec vs `SSL_write` / contiguous buffer
- TLS record差: 1 application-data recordか複数recordか
- HTTP/2 frame差: HEADERS/DATAが同じsend cycleで出ているか、別のflush boundaryに分かれているか
- copy戦略差: scatter/gatherでcopyしないのか、php-grpc-lite側でcontiguous bufferへcopyしてから `SSL_write` しているのか

0.0.5では `send_pending_h2_frames()` の1回の `nghttp2_session_send()` 内でTLS write coalescingを行うため、同一 `nghttp2_session_send()` に出るHEADERS/DATAは1回の `SSL_write` へまとまる設計になっている。

## スコープ

- real Spanner workloadで、RPC boundary marker付きstraceを取り、HEADERS/DATAが分割writeされるか確認する。
- 39B writeが現在RPCのHEADERSなのか、前後のHTTP/2 control frameなのかを切り分ける。
- ext-grpcの `sendmsg(... msg_iovlen=2)` をscatter/gather送信として扱い、php-grpc-liteが同等の送信単位を作れているかを確認する。
- `sendmsg` そのものに寄せるべきか、OpenSSL `SSL_write` に渡す前のcontiguous coalescingで十分かを分けて判断する。
- 現行コード上のflush境界を確認する。

## 非スコープ

- ext-grpcの実装模倣。
- `sendmsg` 化そのものを目的化すること。OpenSSL `SSL_write` を使う限り、syscall APIは `write` になるため、まずはTLS record / flush boundary / copy costのどれが実差かを特定する。
- ローカルTLS test-serverだけを根拠にした採否判断。

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

このため、通常のsmall/medium unary requestでHEADERSとDATAが同一 `nghttp2_session_send()` 内に出る限り、HEADERS/DATAは1 TLS writeにまとまる。ただし、これはコード上の期待値とローカルTLS test-serverでの確認に過ぎず、issue #5で報告されたreal Spanner workloadを反証するものではない。

### scatter/gather観点

現行php-grpc-liteはOpenSSL `SSL_write` を使っているため、ext-grpcのような `sendmsg(... msg_iovlen=2)` は出ない。代わりに `send_callback()` が受け取ったnghttp2の複数chunkをconnection-local contiguous bufferへcopyし、`nghttp2_session_send()` の終端で1回 `SSL_write` する。これはscatter/gatherではなくcopy-based coalescingである。

したがって、実Spannerで確認すべき差は次の順序になる。

1. php-grpc-liteがHEADERS/DATAを本当に2 TLS recordへ分けているか。
2. 分かれているなら、別々の `nghttp2_session_send()` / flush boundaryに分かれているのか、coalescing buffer capacityやlarge-payload bypassで分かれているのか。
3. 分かれていないなら、ext-grpcのscatter/gatherとの差はsyscall表現差であり、latency差の主因候補からは外す。
4. 1 TLS recordにまとまっていてもCPU差が残る場合、copy-based coalescingの追加copy costが問題になるかをCPU profileで見る。

## 現時点の解釈

- ローカルTLS test-serverでは、0.0.5 currentの測定対象RPC requestは1 `write` にまとまっていた。
- しかしissue #5はreal Spanner workloadでの報告であり、ローカル結果だけでは不十分。
- `write(39)` が前後のHTTP/2 control frameである可能性は仮説に留める。
- 次は実Spannerで同じmarker付きtraceを取り、GitHub issue #5の観測そのものに対して確認する。

## 次の確認候補

1. real Spanner workloadでrequest boundary markerをstderrなどに出し、marker前後の `write(39)` が前RPC由来か現在RPC由来か確認する。
2. `SSLKEYLOGFILE` + Wireshark/tshark、またはnghttp2 frame callbackの一時診断で、39B recordのHTTP/2 frame typeを特定する。
3. ext-grpcのscatter/gather送信とphp-grpc-liteのcopy-based coalescingを、TLS record数・HTTP/2 frame境界・CPU copy costに分解して比較する。
4. 39B control frameを次RPCの前ではなくresponse処理直後に必ずflushしている現状が問題になるかを確認する。ただしこれは既に現状の挙動で、request HEADERS/DATA coalescingとは別問題。
5. real Spannerでまだlatency差が残るなら、TLS record数ではなく、preflight `SSL_peek`、poll strategy、Spanner server pacing、GAX request lifecycleを別issueで切り分ける。

## 判断

- Status: Open
- 判断: 未完了。ローカルTLS test-serverでの確認は参考情報であり、issue #5のreal Spanner報告に対する再現確認にはならない。
- 優先度: 最優先でreal Spanner marker付きtraceを取り直す。

## 訂正 2026-05-18

GitHub issue #5へ最初に投稿したコメントは、ローカルTLS test-serverでの確認を強く解釈しすぎていた。issue #5の報告対象はreal Spanner workloadであり、ローカルで再現しないことは反証にならない。GitHub issueには訂正コメントを追加済み。
