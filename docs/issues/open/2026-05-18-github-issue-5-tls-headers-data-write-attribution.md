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

この節は履歴として残すが、issue #5の判断材料からは除外する。issue #5はreal Spanner TLS workloadの報告であり、ローカルTLS test-serverの単発unary結果では反証にも解決にもならない。

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

### real Spanner marker trace

`SPANNER_EMULATOR_HOST` / `LARAVEL_SPANNER_EMULATOR_HOST` を空にし、`vast-falcon-165704 / bench / laravel-bench-db` のreal Spannerへ接続した。最初のemulator向きtraceと、stale session cacheで失敗したtraceは破棄した。採用したtraceでは接続先が `172.217.221.95:443` であることを確認した。

`selectOneRow()` では、warmup後にstderr markerで測定対象を囲み、次の構造を確認した。

```text
===MEASURE_SELECT_BEGIN===
read(fd)=EAGAIN
write(fd, ..., 596) = 596
read(fd)=EAGAIN
ppoll(...) <0.019075>
read(fd, ..., 16713) = 450
write(fd, ..., 39) = 39
read(fd)=EAGAIN
write(fd, ..., 503) = 503
read(fd)=EAGAIN
ppoll(...) <0.019258>
read(fd, ..., 16713) = 208
write(fd, ..., 39) = 39
===MEASURE_SELECT_END===
```

`mixedTransaction()` では、測定区間内に複数のSpanner RPCが順に発生し、各RPCのrequest送信は1つのlarge writeとして見えた。

```text
===MEASURE_MIXED_BEGIN===
read(fd)=EAGAIN
write(fd, ..., 665) = 665
...
read(fd, ..., 16713) = 278
write(fd, ..., 39) = 39
read(fd)=EAGAIN
write(fd, ..., 502) = 502
...
read(fd, ..., 16713) = 266
write(fd, ..., 39) = 39
read(fd)=EAGAIN
write(fd, ..., 603) = 603
...
write(fd, ..., 656) = 656
...
write(fd, ..., 596) = 596
...
write(fd, ..., 728) = 728
...
write(fd, ..., 502) = 502
...
===MEASURE_MIXED_END===
```

解釈:

- markerはPHPアプリケーション側のmethod boundaryであり、HTTP/2 frame boundaryやgRPC stream boundaryではない。
- `selectOneRow()` / `mixedTransaction()` は内部で複数RPCを順に発行するため、`read(response)` の直後に見える `write(39)` は次RPCのHEADERSである可能性を除外できない。
- したがって、このtraceだけでは `39B write` が前RPCのHTTP/2 control frameなのか、次RPCのHEADERS frameなのかを判定できない。
- `39B write` + `large write` のパターン自体はreal Spannerでも見えている。未解明点は報告の有無ではなく、39B recordに含まれるHTTP/2 frame typeと、large writeとのstream_id対応である。
- frame typeは、straceだけではなく、nghttp2 frame callback診断またはTLS復号traceで直接確認する必要がある。

### real Spanner h2 write trace

`GRPC_LITE_H2_WRITE_TRACE=1` の一時診断を入れ、real Spannerで `selectOneRow()` と `mixedTransaction()` を再計測した。診断では、nghttp2の送信frame、`send_callback()` chunk、coalescing buffer flush、`SSL_write` plaintext length、strace上のTLS record writeを同じstderr/strace streamに出した。

`selectOneRow()` の測定区間では、stream 35 と stream 37 の2つのoutbound RPCが発生した。stream 35 のrequest送信は以下の構造だった。

```text
===MEASURE_SELECT_BEGIN===
session_send_begin current_stream_id=35
send_callback_frame type=HEADERS stream_id=35 frame_len=258 chunk_len=267
send_callback_frame type=DATA    stream_id=35 frame_len=291 chunk_len=300
session_send_after_nghttp2 buffered=567
flush_buffer len=567
ssl_write_attempt len=567
write(TLS socket, ..., 589) = 589
ssl_write_done requested_len=567 written=567
```

その後、同じstream 35のresponse処理後に見えていた39B TLS writeの中身は `PING ACK` だった。

```text
session_send_begin current_stream_id=35
send_callback_frame type=PING flags=0x01 stream_id=0 frame_len=8 chunk_len=17
session_send_after_nghttp2 buffered=17
flush_buffer len=17
ssl_write_attempt len=17
write(TLS socket, ..., 39) = 39
ssl_write_done requested_len=17 written=17
```

続くstream 37のrequest送信も、HEADERSとDATAは同一flush / 同一 `SSL_write` にまとまっていた。

```text
session_send_begin current_stream_id=37
send_callback_frame type=HEADERS stream_id=37 frame_len=248 chunk_len=257
send_callback_frame type=DATA    stream_id=37 frame_len=208 chunk_len=217
session_send_after_nghttp2 buffered=474
flush_buffer len=474
ssl_write_attempt len=474
write(TLS socket, ..., 496) = 496
ssl_write_done requested_len=474 written=474
```

`mixedTransaction()` でも同じ構造だった。例として、stream 53、55、57、63、65では、それぞれHEADERSとDATAが同一 `nghttp2_session_send()` 内でbufferされ、1回の `SSL_write` へflushされていた。一方、各response処理後に見える39B TLS writeは、いずれもplaintext 17Bの `PING ACK` だった。

```text
session_send_begin current_stream_id=53
send_callback_frame type=HEADERS stream_id=53 frame_len=258
send_callback_frame type=DATA    stream_id=53 frame_len=362
flush_buffer len=638
ssl_write_attempt len=638
write(TLS socket, ..., 660) = 660

session_send_begin current_stream_id=53
send_callback_frame type=PING flags=0x01 stream_id=0 frame_len=8
flush_buffer len=17
ssl_write_attempt len=17
write(TLS socket, ..., 39) = 39

session_send_begin current_stream_id=55
send_callback_frame type=HEADERS stream_id=55 frame_len=248
send_callback_frame type=DATA    stream_id=55 frame_len=208
flush_buffer len=474
ssl_write_attempt len=474
write(TLS socket, ..., 496) = 496
```

確定したこと:

- real Spanner測定区間で見えていた `39B write` は、今回確認した範囲では `PING ACK` であり、gRPC request HEADERSではない。
- request HEADERSとDATAは、今回確認したreal Spannerの各outbound RPCでは同じ `nghttp2_session_send()` 内に出ており、coalescing buffer経由で1回の `SSL_write` にまとまっている。
- strace上のlarge TLS writeは、plaintextのHEADERS frame + DATA frameにTLS record overheadが乗ったものだった。例: plaintext 567B -> socket write 589B、plaintext 474B -> socket write 496B。
- issue #5で報告された `39B write` + `large write` の観測自体は正しい。ただし、39Bを「同一RPCのHEADERS」と解釈するのは今回のframe traceでは支持されない。
- 現時点の問題候補は、HEADERS/DATA splitではなく、Spanner serverからのPINGに対するACKがresponse後に単独TLS recordとして出ること、またはそのACK schedulingがext-grpcと異なること。

### ext-grpc 1.58.0 optimized real Spanner strace

報告条件に近づけるため、既にビルド済みの `var/official-ext-grpc-so/1.58.0-optimized/grpc.so` を `php -n -d extension=protobuf.so -d extension=/workspace/var/official-ext-grpc-so/1.58.0-optimized/grpc.so` で明示ロードし、同じreal Spanner Laravel経路をstraceした。profileは `-O3 -flto -fno-semantic-interposition`。

`selectOneRow()` 測定区間では、2つのoutbound requestに対してlarge sendが2回、そのresponse後にsmall sendが2回見えた。

```text
===MEASURE_SELECT_BEGIN===
sendmsg(..., iov_len=1094) = 1094
recvmsg(...)=464
sendmsg(..., iov_len=52) = 52
sendmsg(..., iov_len=1011) = 1011
recvmsg(...)=208
sendmsg(..., iov_len=52) = 52
===MEASURE_SELECT_END===
```

`mixedTransaction()` 測定区間では、複数outbound requestの間にsmall sendが継続して見えた。サイズは主に52Bで、一部48B/39Bだった。

```text
===MEASURE_MIXED_BEGIN===
sendmsg(..., iov_len=1164) = 1164
recvmsg(...)=247
sendmsg(..., iov_len=52) = 52
sendmsg(..., iov_len=1011) = 1011
recvmsg(...)=189
sendmsg(..., iov_len=52) = 52
sendmsg(..., iov_len=1102) = 1102
recvmsg(...)=56
recvmsg(...)=402
sendmsg(..., iov_len=52) = 52
sendmsg(..., iov_len=1155) = 1155
recvmsg(...)=335
sendmsg(..., iov_len=48) = 48
recvmsg(...)=97
sendmsg(..., iov_len=39) = 39
sendmsg(..., iov_len=1095) = 1095
...
===MEASURE_MIXED_END===
```

php-grpc-liteの同じ測定区間では、small sendは全て39Bだった。

```text
select: write sizes = 589, 39, 496, 39
mixed:  write sizes = 660, 39, 496, 39, 598, 39, 651, 39, 591, 39, 721, 39, 496, 39
```

この比較から言えること:

- ext-grpc 1.58.0 optimizedでも、real Spannerのresponse後にsmall TLS sendは単独で発生している。
- したがって、small TLS recordが単独で出ること自体はphp-grpc-lite固有ではない。
- php-grpc-liteではframe traceによりsmall 39Bが `PING ACK` だと確認済みだが、ext-grpc側はTLS復号やC-core frame traceを取っていないため、52B/48B/39Bのframe typeは未確定。
- ext-grpc側のlarge sendは `sendmsg` として見えるが、今回の1.58.0 optimized traceではsyscall上の `msg_iovlen` は1だった。issue報告の `iovlen=2` は別条件または別RPC形状で発生している可能性がある。
- ext-grpcはbackground transport thread上でsend/recvしており、php-grpc-liteのPHP request thread同期I/Oとはscheduling modelが異なる。この差はCPU/latencyには効き得るが、今回のtraceだけでsmall sendを「php-grpc-lite固有の余計なwrite」とは言えない。

## 現時点の解釈

- ローカルTLS test-server結果は判断材料から外す。
- real Spannerで `39B write` + `large write` のパターンは実在する。
- frame/TLS write対応を直接確認した結果、今回の測定対象では39B writeは `PING ACK` であり、HEADERS/DATA splitではなかった。
- request HEADERSとDATAは同一stream_idで同じflushにまとまっていた。
- ext-grpc 1.58.0 optimizedでもsmall sendは単独で出るため、small TLS recordそのものはphp-grpc-lite固有の欠陥とは言えない。
- ext-grpcとの差を論じるなら、HEADERS/DATA coalescingではなく、PING ACK/control frame scheduling、threaded transport model、TLS record sizing、またはC-core側のHTTP/2 keepalive/control-frame処理との違いとして扱うべき。

## 次の確認候補

1. php-grpc-lite側で、unary receive loopの `nghttp2_session_mem_recv()` 後に毎回 `send_pending_h2_frames()` を呼ぶ現状を、server streaming同様 `nghttp2_session_want_write()` gateへ寄せられるか確認する。これは空の `session_send` 呼び出し削減であり、PING ACKそのものは残る。
2. PING ACKを次requestとcoalesceする設計がHTTP/2の「ACK without delay」に反しないか確認する。反するなら採用しない。
3. CPU差が残る場合は、PING ACK単独writeのsyscall/TLS overhead、preflight read、poll strategy、GAX request lifecycleを別issueで切り分ける。

## 判断

- Status: Open
- 判断: `39B write` のframe type確認は完了。今回のreal Spanner traceでは `PING ACK` だった。HEADERS/DATA splitとしては再現していない。ext-grpc 1.58.0 optimizedでもsmall sendは単独で発生しており、small write単独発生だけをphp-grpc-lite固有問題とは扱わない。
- 優先度: 次はphp-grpc-lite側の不要な空 `session_send` を削れるかを確認する。issue #5は未解決だが、解決対象はHEADERS/DATA splitではなくPING ACK / control frame scheduling / receive loop fixed costへ移す。

## 修正 2026-05-18

unary receive loopで `nghttp2_session_mem_recv()` 後に無条件で `send_pending_h2_frames()` を呼んでいた箇所を、server streamingと同様に `nghttp2_session_want_write()` がtrueの場合だけ呼ぶ形へ変更した。

この修正の意味:

- HTTP/2 control frameがpendingの場合は従来通りflushする。
- pending outbound frameがない場合は空の `nghttp2_session_send()` を呼ばない。
- `PING ACK` そのものを遅延・削除・coalesceする修正ではない。
- issue #5で確認した39B small writeを消す修正ではなく、receive loop fixed costを減らす修正である。

検証:

- `./tools/test/check-phpt.sh && ./tools/test/check-c-static-analysis.sh`: PASS
- Domain model self review: `docs/reviews/issues/2026-05-18-issue5-unary-want-write-domain-self-review.md`

## 追加報告 2026-05-19: method-level wait latency

GitHub issue #5に、real Spanner single-concurrency workloadで `Google\ApiCore\Transport\GrpcTransport::startUnaryCall()` に `BEGIN` / `WAIT_BEGIN` / `WAIT_END` markerを入れた追加報告があった。

報告内容:

- `Spanner/Commit` のwait latencyが `ext-grpc 1.58: 13.54ms`、`php-grpc-lite 0.0.5: 22.54ms` で1.66x。
- 60秒のsingle-concurrency harnessで、差はほぼ `Commit` に集中。
- 1 RPC syscall breakdownでは、どちらもsend 1回、recv 1回で、HEADERS/DATA splitはない。
- ext-grpcは `_simpleRequest` / `UnaryCall::start()` 側で同期sendし、php-grpc-liteは `UnaryCall::wait()` のrecv batch側で初めて同期sendする。
- poll waitが `ext-grpc: epoll_pwait 12.4ms`、`php-grpc-lite: ppoll 20.2ms` で、差分はpoll中の待ち時間に見える。

この報告の重要点:

- `39B write` / HEADERS splitの問題ではない。
- CPU fixed costではなく、server responseがclient socket readableになるまでの時間差として観測されている。
- `poll syscallが遅い` というより、pollが待っている対象イベントの発生時刻が違う。`epoll_pwait` vs `ppoll` のsyscall実装差だけで8ms差が出たとは解釈しない。
- ext-grpcとphp-grpc-liteでsend timingが違うことは事実。php-grpc-liteの `Call::startBatch()` はSEND opsを保存するだけで、RECV_STATUS batch時にunary RPC全体を実行している。これは公式ext-grpcの「SEND batchで送信開始する」モデルとは異なる。

### こちらの再計測

同じLaravel Spanner fixtureに一時的に `GRPC_ISSUE5` markerを入れ、real Spanner `transaction_select2_update1_insert1` を10 iteration実行した。Commit windowは各iterationで2回、合計20件。

条件:

- real Spanner: `vast-falcon-165704 / bench / laravel-bench-db`
- action: `transaction_select2_update1_insert1`
- iterations: 10
- native current: `ext/grpc/modules/grpc.so`
- native 0.0.5: `var/tag-so/0.0.5/grpc.so`
- ext-grpc: `var/official-ext-grpc-so/1.58.0-optimized/grpc.so`
- trace: `strace -f -tt -T -yy -e trace=ppoll,poll,epoll_pwait,sendmsg,sendto,write,recvmsg,recvfrom,read`

集計:

| variant | n | BEGIN→WAIT_END mean | WAIT_BEGIN→WAIT_END mean | BEGIN→WAIT_BEGIN mean | poll wait mean | large send |
|---|---:|---:|---:|---:|---:|---|
| php-grpc-lite 0.0.5 | 20 | 24.38ms | 24.20ms | 0.18ms | 23.13ms | 496B mostly |
| php-grpc-lite current | 20 | 24.59ms | 24.42ms | 0.18ms | 23.22ms | 496B mostly |
| ext-grpc 1.58 optimized | 20 | 24.60ms | 21.80ms | 2.80ms | 19.24ms | 1042B mostly |

この再計測では、報告された `BEGIN→WAIT_END` の1.66x差は再現していない。currentと0.0.5も同等だったため、`b482868` の `want_write` gateはこのCommit wait差を説明しない。

一方で、次の差分は再確認できた。

- php-grpc-liteは `BEGIN→WAIT_BEGIN` が非常に短い。これは `Grpc\UnaryCall::start()` / SEND batchではnetwork sendせず、`wait()` 側でsendしているため。
- ext-grpcは `BEGIN→WAIT_BEGIN` が長い。これは `_simpleRequest()` 内の `start()` / SEND batchでnetwork sendを済ませてからPromise wait closureへ入るため。
- ext-grpcの `WAIT_BEGIN→WAIT_END` だけを見ると短く見えるが、`BEGIN→WAIT_END` では今回の環境ではphp-grpc-liteと同等だった。
- php-grpc-liteのCommit large sendはext-grpcより小さい。これはTLS/syscall上の観測であり、HPACK dynamic table / metadata encoding / ext-grpcのTLS record composition差が疑われる。ただし `authorization` metadataを一時的に `NGHTTP2_NV_FLAG_NO_INDEX` にしてもsend sizeやlatencyは変わらなかったため、authorization indexing単独では説明できない。

### socket option確認

同じreal Spanner pathで `setsockopt` / `getsockopt` をtraceした。

- php-grpc-lite: Spanner TLS fdに `TCP_NODELAY` を設定。
- ext-grpc 1.58: `TCP_NODELAY`、`SO_REUSEADDR`、`TCP_INQ` を設定。
- どちらのtraceにも `TCP_QUICKACK` は出ていない。

`TCP_INQ` は受信可能byte数の取得用途であり、response到着時刻そのものを早める説明にはなりにくい。現時点でsocket option差だけを主因とは見ない。

## 現時点の更新解釈 2026-05-19

- issue #5の焦点は、HEADERS/DATA splitから外れた。
- 追加報告の `Commit` 差は重要だが、こちらの同一fixture 10 iterationでは `BEGIN→WAIT_END` の1.66x差は再現しなかった。
- `WAIT_BEGIN→WAIT_END` だけを比較するとext-grpcが短く見えるが、ext-grpcはSEND batchで先に送信しているため、`wait()` 区間だけの比較は実装モデル差を含む。
- php-grpc-liteの「SEND batchで送信せず、RECV batchでunary全体を実行する」実装は、公式ext-grpc互換性としては弱い。latency差の主因かは未確定だが、別issueで「unary SEND batch eager send」対応を検討する価値がある。
- poll waitの差は原因ではなく観測結果。次に見るべきは、request send完了時刻からresponse first byte到着までの差、送信HTTP/2 headers/payload shape、server側のCommit処理時間、またはGAX/Spanner request lifecycle差である。

## 追加報告 2026-05-19: tcpdump + strace相関

GitHub issue #5に、real Spanner FPM single-concurrency harnessで `tcpdump` とmarker付き `strace` を同時に取得した追加報告があった。

報告内容:

- 対象は `Spanner/Commit`。45秒run、warmup除外。
- `ext-grpc 1.58`: n=252、`BEGIN→WAIT_END mean 13.10ms`。
- `php-grpc-lite 0.0.5`: n=172、`BEGIN→WAIT_END mean 21.76ms`。
- `send syscall → outbound TCP packet` は両者とも約0.03ms。
- `inbound TCP data packet → recv syscall` は両者ともsub-ms。
- 差分の大半は `outbound TCP packet → inbound TCP data packet` にあり、`ext-grpc 12.41ms`、`php-grpc-lite 20.65ms`。
- `poll` syscall durationも同じ差分を示し、これはpoll syscall自体ではなく、response packet到着待ちの結果と見える。
- outbound TLS application payloadは、`ext-grpc ~1571B`、`php-grpc-lite ~942B` と報告されている。

この報告で強くなったこと:

- `send syscall` 後のkernel enqueue遅延、response packet到着後のuserland wakeup / recv loop遅延は主因候補から弱くなる。
- `ppoll` と `epoll_pwait` のsyscall実装差だけで8ms差が出ている、という見方はさらに弱くなる。
- 差分はclientがrequest packetを出した後、serverの最初のresponse packetがclient側で観測されるまでの区間に集中している。

ただし、レビュー後の扱いとして、これはまだ「wire RTT」や「serverが遅く返した」と断定できない。

- `outbound TCP packet → inbound TCP data packet` は、network RTTだけでなく、Google frontend受信、TLS復号、HTTP/2/HPACK decode、metadata/auth/routing処理、Spanner Commit処理、server queueing、response生成を含む。
- `first outbound packet` はrequest完了時刻とは限らない。requestが複数TLS record / TCP segmentに分かれる場合、serverが処理可能になる時刻は `last outbound byte` 側になる。
- `ext-grpc ~1571B` と `php-grpc-lite ~942B` のoutbound payload差が大きい。これは単なるサイズ差ではなく、metadata set、HPACK dynamic table、`grpc-timeout`、`:authority`、`:path`、`authorization`、`x-goog-*`、`user-agent`、control frame混入、DATA payload lengthのどれかが違う可能性を示す。
- したがって、現時点で正確に言えるのは「client-observed request-to-first-response-packet latencyに差が集中している」まで。

批判レビュー:

- `docs/reviews/issues/2026-05-19-issue5-wire-rtt-claim-critical-review.md`

次に必要なこと:

1. `first outbound packet` ではなく、対象Commit streamの `request last outbound byte → first inbound response byte` を取る。
2. TLS key logまたはHTTP/2 frame traceで、Commit requestのHEADERS / DATA / control framesをext-grpcとphp-grpc-liteで比較する。
3. metadata set、metadata order、重複、`grpc-timeout`、`:authority`、`:path`、`authorization`、`x-goog-*`、`user-agent`、DATA payload length、HPACK indexed/literal、dynamic table状態を表にする。
4. request直前直後のPING / SETTINGS / WINDOW_UPDATE / ACKが、Commit streamのpacket列に混ざるか確認する。
5. 同一connection warm state、stream id進行、Spanner session warmup、transaction shapeを揃え、複数run・交互順序・分布指標で集計する。

## wire shape完全比較 2026-05-19

追加報告を受けて、real Spannerの同じ `transaction_select2_update1_insert1` 経路で `Spanner/Commit` の送信shapeを比較した。

比較条件:

- 対象: `Spanner/Commit`
- ext-grpc: `1.58.0` optimized build
- php-grpc-lite: current native extension with temporary wire trace
- 実行形態: CLI profile action。FPM 45秒runそのものではないため、latency分布の結論には使わない
- 認証: 同じADC credentials
- Spanner: `vast-falcon-165704 / bench / laravel-bench-db`

### request body

| item | ext-grpc 1.58 | php-grpc-lite | 判断 |
| --- | ---: | ---: | --- |
| protobuf payload | 203B | 203B | 同じ |
| gRPC DATA payload | 203B + 5B prefix | 203B + 5B prefix | 同じ |
| compressed flag | `0` | `0` | 同じ |
| DATA END_STREAM | あり | あり | 同じ |

Commit request body自体は一致している。payload内容やgRPC 5B framingの差でCommit latency差が出ている、という仮説は弱い。

### request metadata

| metadata | ext-grpc 1.58 | php-grpc-lite | 判断 |
| --- | --- | --- | --- |
| `:method` | `POST` | `POST` | 同じ |
| `:scheme` | `https` | `https` | 同じ |
| `:authority` | `spanner.googleapis.com:443` | `spanner.googleapis.com:443` | 同じ |
| `:path` | `/google.spanner.v1.Spanner/Commit` | `/google.spanner.v1.Spanner/Commit` | 同じ |
| `content-type` | `application/grpc` | `application/grpc` | 同じ |
| `te` | `trailers` | `trailers` | 同じ |
| `grpc-timeout` | trace上は `@601491ms` / `@601659ms` | `600000m` | 表記または値が異なる。ext traceの `@` はwire valueとは断定しない |
| `user-agent` | `gcloud-php-legacy/1.104.1 grpc-php/1.58.0 grpc-c/35.0.0 (linux; chttp2)` | `gcloud-php-legacy/1.104.1 grpc-php/0.1.0` | 異なる |
| `grpc-accept-encoding` | `identity, deflate, gzip` | なし | 異なる |
| `x-goog-api-client` | `... grpc/1.58.0 ...` | `... grpc/0.1.0 ...` | 異なる |
| `x-goog-api-client` duplicate | `cred-type/u` | なし | 異なる |
| `x-goog-user-project` | あり | あり | 同じ |
| `x-goog-request-params` | あり | あり | 同じ |
| `x-goog-spanner-route-to-leader` | `true` | `true` | 同じ |
| `google-cloud-resource-prefix` | あり | あり | 同じ |
| `authorization` | あり | あり | 同じ扱い。値は記録しない |

metadata shapeは一致していない。特に重要なのは、ext-grpcが `x-goog-api-client` を2値として送っている一方、php-grpc-liteはCallCredentials plugin由来の `x-goog-api-client: cred-type/u` を落としている点。

php-grpc-lite側では `grpc_lite_merge_call_credentials_metadata()` が `zend_hash_merge(..., overwrite = 0)` でplugin返却metadataを既存metadataへmergeしている。PHP HashTableのstring keyは同名keyを複数保持できないため、既存の `x-goog-api-client` があるとplugin側の同名keyが追加されない。gRPC metadataは同名keyの複数値を許すため、この挙動はwire互換性上の差分として扱う。

### HTTP/2 frame / TLS write shape

| item | ext-grpc 1.58 | php-grpc-lite | 判断 |
| --- | ---: | ---: | --- |
| Commit stream id | `3`, `13` | `3`, `13` | 今回のtraceでは同じ進行 |
| HEADERS compressed length | ext traceでは直接未取得 | `259B`, `252B` | ext側は未確定 |
| DATA frame length | trace上はmessage len `203B` | `208B` | gRPC prefix込みなら同等 |
| outbound TLS write size | `1100B`, `1042B` | `507B`, `500B` | ext-grpcの方が大きい |
| request send syscall duration | sub-ms | sub-ms | 差の主因ではない |
| first response待ち | 単発traceでは揺れ大 | 単発traceでは揺れ大 | latency結論には使わない |

ext-grpcのoutbound TLS write sizeが大きいことは、報告者の `ext-grpc ~1571B` / `php-grpc-lite ~942B` という方向性と一致する。ただし、今回のtraceではext-grpc側のHEADERS compressed blockやHPACK dynamic table状態までは復元していないため、サイズ差の内訳はまだ確定しない。

### 完全比較で確定したこと

- Commit protobuf payloadは同じ。
- gRPC DATA lengthも同じ。
- `first outbound packet -> first inbound packet` の差を、payload差やrequest body差で説明する根拠は弱い。
- request metadata shapeは同じではない。
- php-grpc-liteはCallCredentials plugin由来のduplicate metadataを保持できていない可能性が高い。
- `grpc-accept-encoding`、`user-agent`、`x-goog-api-client` の差も残っている。

### 次の判断

この時点で、`Commit` latency差の説明としてはtransport syscallやpollよりも、まずmetadata wire shape差を潰すべき。

優先順:

1. gRPC metadataとして同名key複数値を保持し、CallCredentials plugin由来のduplicate metadataを落とさない。
2. `grpc-accept-encoding: identity, deflate, gzip` の送信要否を仕様・実務影響から判断する。
3. `user-agent` / `x-goog-api-client` のversion差がSpanner frontend処理に影響するかを切り分ける。ただし公式実装文字列の模倣を目的にしない。
4. その後、FPM 45〜60秒runで `request last outbound byte -> first inbound response byte` を再測定する。

## local tcpdump + strace相関 2026-05-19

報告者の追加報告を軽く扱わないため、こちらでも同じ観測方向の `tcpdump` + `strace` 同時取得を実施した。

実施条件:

- real Spanner
- action: `transaction_select2_update1_insert1`
- iterations: `10`
- ext-grpc: `1.58.0` optimized build
- php-grpc-lite: current native extension
- 実行形態: CLI profile action。FPM 45秒runではない
- pcap: container内 `tcpdump -i any -nn -s 0 'tcp port 443'`
- strace: `write,sendmsg,recvmsg,read,ppoll,epoll_pwait`

この測定では、Commit streamを厳密同定するmarkerやTLS復号はまだない。そのため、報告者と同じ結論強度ではなく、TCP payload packet単位の粗い相関として扱う。

集計方法:

- outbound TCP payload packetから次のinbound TCP payload packetまでを計算。
- TLS handshake付近の先頭pairを除外。
- request候補として、outbound payload length `450B`〜`1600B`、next inbound payload `100B`以上を抽出。
- この候補にはCommit以外のSpanner RPCも含まれ得る。

結果:

| variant | candidates | median | mean | p90 | p95 |
| --- | ---: | ---: | ---: | ---: | ---: |
| php-grpc-lite current | 68 | 21.452ms | 26.055ms | 34.922ms | 36.570ms |
| ext-grpc 1.58 optimized | 62 | 20.947ms | 24.235ms | 33.723ms | 34.154ms |

このローカルCLI条件では、報告された `ext-grpc 12.41ms`、`php-grpc-lite 20.65ms` のような1.6x差は再現していない。packet相関上も両者は近いレンジだった。

ただし、この結果は報告者のFPM 45秒runを否定しない。

- CLI実行であり、FPM worker lifecycle、session warmup、長時間run、worker stateが違う。
- Commitだけを厳密抽出していない。
- TLS復号/HTTP/2 stream idでrequest last byteを特定していない。
- 報告者のn=252 / n=172に対して、こちらは10 iterationの短時間run。

現時点の扱い:

- 報告者の `tcpdump + strace` 観測は、調査対象として維持する。
- こちらの短時間CLI相関では同じ差は出ていないため、差分はFPM/長時間run/worker warm state/metadata shape/Spanner backend揺れのいずれかに依存している可能性がある。
- 次の再現には、FPM single-concurrencyで、Commit markerまたはHTTP/2復号により `Commit request last outbound byte -> first inbound response byte` を直接集計する必要がある。

## FPM single-concurrency再計測 2026-05-19

報告者の条件に寄せるため、FPM + nginx + `hey -z 45s -c 1 -disable-keepalive` でreal Spanner `transaction_select2_update1_insert1` を再計測した。同時にFPM container内で `tcpdump -i any 'tcp port 443'` と全php-fpm workerへの `strace` attachを実施した。

比較対象:

- `ext-grpc 1.58.0 optimized`: `/workspace/var/official-ext-grpc-so/1.58.0-optimized/grpc.so`
- `php-grpc-lite current`: `/workspace/ext/grpc/modules/grpc.so`
- `php-grpc-lite 0.0.5`: `/workspace/var/tag-so/0.0.5/grpc.so`

HTTP load結果:

| variant | requests | rps | avg | p50 | p90 | p95 | max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| php-grpc-lite current | 202 | 4.4775 | 223.3ms | 206.7ms | 318.2ms | 337.3ms | 468.0ms |
| ext-grpc 1.58 optimized | 224 | 4.9718 | 201.1ms | 193.8ms | 215.4ms | 251.0ms | 543.9ms |
| php-grpc-lite 0.0.5 | 235 | 5.2051 | 192.1ms | 181.8ms | 237.9ms | 257.6ms | 404.0ms |

このrunでは、currentはext-grpc 1.58より約11%低いthroughput、約22ms遅い平均応答時間になった。一方で、報告対象に近い0.0.5はこのrunではext-grpc 1.58より速く、報告者の `ext n=252 / lite n=172` という大きな差は再現しなかった。

この結果は、報告者の結果を否定しない。理由は以下。

- こちらは1 runずつで、交互複数runではない。
- 報告者の環境、GCP frontend/backend割当、host CPU、FPM設定、SO build条件、run順序はまだ完全には一致していない。
- 0.0.5とcurrentで結果が逆転しており、run間揺れまたはbuild/runtime条件差が十分に残っている。
- `strace` attachと`tcpdump`を同時に入れているため、通常負荷試験とは絶対値が変わる。

tcpdump packet相関:

Commitを厳密同定するmarkerやTLS復号はまだないため、ここではTCP payload packet単位の粗い候補抽出に留める。

抽出条件:

- outbound TCP payload packetから次のinbound TCP payload packetまでを計算。
- request候補として、outbound payload length `450B`〜`1700B`、next inbound payload `100B`以上を抽出。
- Commit以外のSpanner RPCを含む。

| variant | candidates | median | mean | p90 | p95 | p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| php-grpc-lite current | 1313 | 23.703ms | 26.673ms | 38.019ms | 39.579ms | 42.659ms |
| ext-grpc 1.58 optimized | 1034 | 23.281ms | 25.096ms | 33.997ms | 35.096ms | 37.481ms |
| php-grpc-lite 0.0.5 | 1494 | 22.802ms | 23.530ms | 29.964ms | 30.969ms | 35.691ms |

packet相関では、currentはext-grpcよりtailが悪いが、報告者の `12.41ms vs 20.65ms` ほどの大きな差はこのrunでは見えていない。0.0.5はpacket相関でもext-grpcより良い結果になった。

packet length bucketの参考:

- php-grpc-liteの既存wire traceでは、`Spanner/Commit` はwarm状態でTLS write sizeがおおむね `500B` 前後になる。
- ext-grpc 1.58では、同じ `Spanner/Commit` が `1042B` 前後で観測される。
- この対応で見ると、手元FPM runのCommit相当bucketは以下。

| variant | bucket | n | median | mean | p90 |
| --- | ---: | ---: | ---: | ---: | ---: |
| php-grpc-lite current | `497B` | 337 | 23.008ms | 24.264ms | 24.764ms |
| ext-grpc 1.58 optimized | `1042B` | 261 | 23.146ms | 23.332ms | 25.251ms |
| php-grpc-lite 0.0.5 | `497B` | 389 | 23.111ms | 23.744ms | 25.518ms |

このbucket対応が正しければ、手元FPM runではCommit単体の `outbound payload -> next inbound payload` はext-grpcとphp-grpc-liteでほぼ同等だった。currentのHTTP response全体が遅かった主な見え方は、Commit相当bucketではなく、`591B/592B/723B` など別RPC形状のtail悪化に出ている。

ただし、これはTLS復号やmethod markerによる確定ではない。packet sizeによる推定であり、次のdiagnosticでmethod単位に確定する。

現時点で言えること:

- `php-grpc-lite current` では、FPM single-concurrencyのmixed transactionでext-grpc 1.58より遅いrunを手元でも観測した。
- ただし、報告者の0.0.5に対する大きな差は、今回のFPM 45秒runでは再現していない。
- 粗いTCP packet相関だけでは、差分が特定のCommit RPCにあるとはまだ言えない。むしろ手元runではCommit相当bucketは同等に見える。
- 次はHTTP response全体ではなく、RPC method単位、特に `Spanner/Commit` 単位で抽出できるmarkerまたはHTTP/2復号相当が必要。

次の作業:

1. FPM runでRPC method名、phase、pid、時刻を出せるdiagnosticを用意し、`Commit` だけを抽出する。
2. `request first outbound byte`、`request last outbound byte`、`first inbound response byte`、`recv syscall`、`wait end` をCommit単位で集計する。
3. ext-grpc 1.58 / php-grpc-lite current / php-grpc-lite 0.0.5 を交互順序で複数runする。
4. metadata差修正は重要だが、原因特定前に主因扱いしない。

## metadata alignment実験 2026-05-19

wire shape差のうち、まずCallCredentials plugin由来のduplicate metadata保持を修正した。加えて、`grpc-accept-encoding: identity, deflate, gzip` を一時的な実験フックで送る条件を作り、real Spanner FPM single-concurrencyで比較した。

条件:

- endpoint: `transaction_select2_update1_insert1`
- load: `hey -z 45s -c 1 -disable-keepalive`
- backend: real Spanner `vast-falcon-165704 / bench / laravel-bench-db`
- FPM: `fpm-lifecycle-profile`
- SO: current working tree build
- accept-encoding条件: 一時的に `GRPC_LITE_EXPERIMENTAL_ACCEPT_ENCODING=identity, deflate, gzip` を見てrequest headerを追加するpatchで測定。測定後、この実験フックは本実装から戻した。

HTTP load結果:

| variant | run | requests | rps | avg | p50 | p90 | p95 | p99 | max |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| duplicate metadata fix only | warm run 1 | 252 | 5.5897 | 178.9ms | 176.7ms | 191.4ms | 207.3ms | 230.4ms | 289.2ms |
| duplicate metadata fix only | recreated, run 1 | 240 | 5.3136 | 188.2ms | 178.2ms | 224.0ms | 240.1ms | 344.9ms | 539.9ms |
| duplicate metadata fix only | recreated, run 2 | 250 | 5.5361 | 180.6ms | 177.9ms | 187.6ms | 193.1ms | 301.7ms | 330.8ms |
| duplicate + `grpc-accept-encoding` | recreated, run 1 | 218 | 4.8409 | 206.6ms | 182.8ms | 334.2ms | 391.2ms | 461.1ms | 512.4ms |
| duplicate + `grpc-accept-encoding` | same FPM warm run 2 | 247 | 5.4769 | 182.6ms | 179.9ms | 195.1ms | 198.5ms | 240.3ms | 299.4ms |

無効run:

- FPM再作成直後に5秒sleepだけで開始したrunは、502が大量に混ざったため破棄した。

解釈:

- `grpc-accept-encoding` をext-grpcと同じ値に揃えても、warm runではduplicate-onlyとの差は小さい。
- recreated直後のrunではtail悪化が出たが、同じFPMプロセスを再利用したwarm runでは消えたため、`grpc-accept-encoding` そのものが安定した改善要因/悪化要因とはまだ言えない。
- php-grpc-liteはresponse compressionを実装していない。`grpc-accept-encoding: identity, deflate, gzip` をproduction defaultで送ると、serverがgzip responseを返した場合にclient側でUNIMPLEMENTEDになる可能性がある。このため、今回の結果だけでは本採用しない。
- ただし、報告者環境との差を詰める観測項目としては有効なので、必要ならmethod単位diagnosticの次段で再度含める。

## 訂正 2026-05-18

GitHub issue #5へ最初に投稿したコメントは、ローカルTLS test-serverでの確認を強く解釈しすぎていた。issue #5の報告対象はreal Spanner workloadであり、ローカルで再現しないことは反証にならない。GitHub issueには訂正コメントを追加済み。

## 0.0.6 trace build local FPM measurement 2026-05-19

0.0.6へ入れる調査用traceを使い、こちらで再現可能なreal Spanner + FPM single-concurrency条件を再計測した。

条件:

- real Spanner: `vast-falcon-165704 / bench / laravel-bench-db`
- endpoint: `transaction_select2_update1_insert1`
- load: `hey -z 30s -c 1 -disable-keepalive`
- FPM: `fpm-lifecycle-profile`, `cpus: 4.0`, startup session warmupあり
- trace: `LARAVEL_SPANNER_TRACE=1`
- php-grpc-lite currentのみ `GRPC_LITE_TRACE_FILE` による拡張内 `rpc.end` も取得
- order: current -> ext-grpc 1.58 optimized -> 0.0.5 -> ext-grpc 1.58 optimized -> current
- logs: `var/bench-results/issue5-006-local-20260519-084435/`

HTTP load結果:

| variant | runs | ok total | rps mean | avg ms mean | p50 ms mean | p90 ms mean | p95 ms mean | p99 ms mean |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| php-grpc-lite current | 2 | 293 | 4.866 | 205.7 | 192.6 | 268.6 | 279.9 | 316.2 |
| ext-grpc 1.58 optimized | 2 | 302 | 5.017 | 199.3 | 190.3 | 224.8 | 293.2 | 401.0 |
| php-grpc-lite 0.0.5 | 1 | 144 | 4.784 | 209.0 | 189.1 | 291.2 | 313.4 | 456.4 |

currentのgrpc-lite拡張内RPC elapsed:

| run | method | n | avg us | p50 us | p90 us | p95 us | p99 us | max us |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | `Spanner/Commit` | 286 | 24816 | 23517 | 29121 | 34563 | 43570 | 50935 |
| 1 | `Spanner/ExecuteStreamingSql` | 713 | 29415 | 25942 | 35314 | 41046 | 121025 | 127196 |
| 5 | `Spanner/Commit` | 304 | 24029 | 23503 | 26445 | 27948 | 34194 | 44071 |
| 5 | `Spanner/ExecuteStreamingSql` | 758 | 27644 | 25883 | 34416 | 40719 | 64166 | 133382 |

app step比較の代表値:

| variant/run | `http.bench` p50/p95 us | `mixed.main_transaction` p50/p95 us | `mixed.pre_insert.transaction` p50/p95 us |
|---|---:|---:|---:|
| current run 1 | 186594 / 296471 | 133778 / 152728 | 49862 / 158626 |
| ext 1.58 run 2 | 181150 / 242418 | 132080 / 147196 | 47467 / 94819 |
| 0.0.5 run 3 | 182483 / 299523 | 131735 / 150049 | 49653 / 152073 |
| ext 1.58 run 4 | 184031 / 260163 | 133808 / 155721 | 48928 / 94782 |
| current run 5 | 184138 / 234331 | 131913 / 145297 | 50005 / 91720 |

解釈:

- 手元のreal Spanner + FPM c1条件では、報告者環境のような `ext-grpc 1.58` に対するphp-grpc-lite 0.0.5の大きな劣後は再現しなかった。
- current trace buildはext-grpc 1.58 optimizedと同じレンジにあり、2 run平均ではRPSが約3%低い程度だった。ただし30秒runかつSpanner backendの揺れを含むため、優劣の結論には使わない。
- currentの拡張内traceでは、`Spanner/Commit` はp50約23.5ms、p99約34〜44msだった。報告者の `Commit wait` 差が出る環境では、この `rpc.end` とapp stepを同時に取ることで、差がCommit RPC内に寄るか、app/transaction stepに寄るかを切り分けられる。
- ext-grpc内部のRPC completionはこのリポジトリからは取れないため、共通比較はapp step、php-grpc-lite詳細は0.0.6 traceの `rpc.end` として扱う。

レビュー:

- 報告者環境の差をこちらで再現した、とは言えない。
- ただし、こちらで実施可能な範囲では、real Spanner/FPM/official ext-grpc 1.58 optimized/current/0.0.5の同一endpoint比較と、currentのRPC method-level traceは取得できた。
- 次に必要なのは、同じ0.0.6 trace buildを報告者環境で実行してもらい、こちらと同じ観測点で差が出るか確認すること。

## Reporter follow-up after 0.0.6 trace request 2026-05-19

GitHub issue #5に、報告者側で0.0.6を使った追加調査結果が返信された。

報告内容:

1. GAX levelの `$options['headers']` はext-grpc 1.58とphp-grpc-lite 0.0.6で同一。
   - `x-goog-api-client`
   - `User-Agent`
   - `x-goog-request-params`
   - `x-goog-spanner-route-to-leader`
   - `google-cloud-resource-prefix`
2. `ss -tipn` で見たkernel TCP RTT / minrtt / mss / cwndは両者で同等。
   - TCP RTTは数十〜数百µsで、Commitの8ms差とは桁が違う。
3. gdbでphp-grpc-lite側の `SSL_write` plaintextを見た結果、HEADERS + DATAは同一 `SSL_write` にcoalesceされている。
   - 例: HEADERS frame `length=671, type=HEADERS, END_HEADERS, stream=0x427`
   - 残りが同じcallのDATA frame。
4. `send_syscall -> outbound packet` と `inbound packet -> recv syscall` は両者で同等。
5. 残る差分は `outbound packet -> server first response packet` に集中しており、報告者環境ではext-grpcが約12ms、php-grpc-liteが約20ms。

報告者の暫定結論:

- PHP layer、syscall set、kernel TCP path、HEADERS/DATA splitでは説明できない。
- Spanner serverがphp-grpc-liteのCommit requestに対して約8ms遅くfirst response packetを返しているように見える。
- 未確認の残差は、実際にwireへ出ているHTTP/2/HPACK encoded HEADERSやframe bytesの構造差。

こちらの更新解釈:

- GAX layer metadata差は主因候補から弱くなった。ただしgRPC binding layerで追加されるmetadata、HPACK indexing、header order、dynamic table state、control frame混入はまだ未比較。
- TCP network RTTやclient wakeupは主因候補からさらに弱くなった。
- HEADERS/DATA splitはほぼ否定された。
- 次に見るべき一次データは、Commit requestのHTTP/2 frame sequenceとHPACK-encoded HEADERS blockである。

次の対応候補:

1. php-grpc-liteにopt-inのwire diagnosticを追加し、outbound HTTP/2 frame metadataとHEADERS block bytesをbase64またはhexでtraceできるようにする。
2. ext-grpc側は同等traceが直接取れないため、報告者側で `GRPC_TRACE=http,api,transport_security` 等が有効か確認してもらう。ただしC-core/BoringSSL静的リンクのため、完全なplaintext frame bytes取得は再ビルドなしでは難しい可能性がある。
3. 比較対象を「GAX headers」ではなく「gRPC binding後のactual HTTP/2 request」に移す。
4. もしgrpc-liteのwire bytesしか取れない場合でも、Commit requestに含まれるpseudo headers、regular metadata、HPACK flags、DATA length、control framesの自明な異常がないかを確認する。

## php-grpc-lite wire diagnostic追加 2026-05-19

報告者側の追加調査で残差がactual HTTP/2/HPACK wire shapeに絞られたため、php-grpc-lite側でopt-inのwire diagnosticを追加した。

有効化:

```sh
GRPC_LITE_TRACE_FILE=/path/to/grpc-lite-trace.ndjson
```

追加されるevent:

- `wire.connection_preface`
  - HTTP/2 client connection preface送信。
- `wire.request_header`
  - `nghttp2_submit_request()` に渡すgRPC binding後のactual request header list。
  - `stream_id`, `rpc_method`, `index`, `name`, `value_len`, `flags`, `sensitive` を記録。
  - `authorization`, `x-goog-user-project`, `x-goog-request-params`, `google-cloud-resource-prefix` は値を出さず `value_sha256` のみ記録。
- `wire.frame_out`
  - `send_callback()` に出たoutbound HTTP/2 frame単位のmetadata。
  - `stream_id`, `frame_type`, `frame_type_id`, `flags`, `frame_payload_len`, `chunk_len`, `rpc_method` を記録。
  - HEADERSでは `header_block_len` も記録。

追加option:

```sh
GRPC_LITE_TRACE_WIRE_BYTES=1
```

- HEADERS frame payloadのHPACK encoded header blockを `header_block_hex` として出す。
- これはauthorization等のliteral valueをHPACK block内に含む可能性があるため、公開issueへ貼らない。必要な場合だけprivate共有またはlocal解析に使う。

real Spanner smoke:

- endpoint: `transaction_select2_update1_insert1`
- 1 request, HTTP 200。
- `Spanner/Commit` 2件で `wire.request_header` と `wire.frame_out` を確認。
- Commit request header listには `x-goog-api-client` のbase valueと `cred-type/u` のduplicate valueが両方出ている。
- Commit frameは `HEADERS` と `DATA` が同じRPC method / stream_idで記録された。
- 例: `Spanner/Commit` stream 3で `HEADERS frame_payload_len=259`, `DATA frame_payload_len=208`。

判断:

- これでphp-grpc-lite側は、GAX layerではなくgRPC binding後のactual header list、HTTP/2 frame sequence、HPACK header block lengthまで観測できる。
- ext-grpc側の同等plaintext/HPACK取得は引き続き難しいが、報告者環境でgrpc-liteのwire traceを取れば、少なくともgrpc-lite request shapeの異常、重複metadata、sensitive header length/digest、header order、frame length、control frame混入は確認できる。
