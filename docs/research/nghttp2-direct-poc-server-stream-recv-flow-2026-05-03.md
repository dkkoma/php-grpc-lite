# nghttp2 direct PoC: server streaming receive flow-control breakdown (2026-05-03)

## 目的

server streaming large response の p99 tail について、前回残した候補を切る。

対象候補は以下。

1. nghttp2 direct PoC の receive loop / poll scheduling。
2. HTTP/2 receive-side flow-control / WINDOW_UPDATE。
3. Go server の send 側 pacing / HTTP/2 write buffer。
4. ext-grpc と php-grpc-lite / nghttp2 PoC の receive-side 挙動差。

## 追加計測

`poc/nghttp2-client-ext/nghttp2_poc.c` に以下を追加した。

- client が送信した WINDOW_UPDATE frame 数 / increment / 初回・最終送信時刻。
- `recv()` syscall 合計 / 最大時間。
- `nghttp2_session_mem_recv()` 合計 / 最大時間。
- 受信後 `nghttp2_session_send()` 合計 / 最大時間。
- `poll()` wait 合計 / 最大時間 / ready event 数。
- client receive stream / connection window を広げる `--recv-stream-window-size` / `--recv-connection-window-size`。

Go test-server の `benchStatsHandler` は streaming でも `OutPayload` の first / last / count を trailers に出すようにした。

`tools/phase2/streaming-diagnostic.php` を追加し、php-grpc-lite と ext-grpc の streaming response でも同じ server stats trailer を保存できるようにした。

## PoC: client receive window sweep

条件: nghttp2 PoC、server streaming、response body discard、no-copy、poll loop。

| case | window | p50 | p99 | poll wait p99 | WINDOW_UPDATE sent p99 | server last OutPayload p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 10×100KiB | default | 752.0μs | 4197.0μs | 4012.0μs | 57 | 4012.1μs |
| 10×100KiB | 16MiB | 505.0μs | 3668.0μs | 3605.0μs | 1 | 3424.2μs |
| 1×1MiB | default | 671.0μs | 4970.0μs | 4622.0μs | 65 | 3327.2μs |
| 1×1MiB | 16MiB | 445.0μs | 4052.0μs | 3948.0μs | 1 | 3196.3μs |

判断:

- receive window を 16MiB に広げると WINDOW_UPDATE は 57/65 回から 1 回に減り、p50/p99 が改善した。
- `poll_wait_p99` は total p99 に近いが、これは client CPU が詰まっている時間ではなく、次の DATA / trailers を待っている時間である。
- `nghttp2_session_mem_recv()` p99 は 10×100KiB で 20μs、1×1MiB で 17μs 程度だった。nghttp2 decode callback 自体は主因ではない。
- `recv()` syscall p99 も 90〜113μs 程度で、tail の主要因ではない。

## PoC: server write buffer

条件: nghttp2 PoC、client receive window 16MiB、server `TEST_SERVER_GRPC_WRITE_BUFFER_SIZE=0`。

| case | server write buffer | p50 | p99 | poll wait p99 | WINDOW_UPDATE sent p99 | server last OutPayload p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 10×100KiB | default | 505.0μs | 3668.0μs | 3605.0μs | 1 | 3424.2μs |
| 10×100KiB | 0 | 559.0μs | 3428.0μs | 3358.0μs | 1 | 3193.1μs |
| 1×1MiB | default | 445.0μs | 4052.0μs | 3948.0μs | 1 | 3196.3μs |
| 1×1MiB | 0 | 467.0μs | 3513.0μs | 3440.0μs | 1 | 3155.4μs |

判断:

- server write buffer 条件も p99 に効く。ただし、client receive window を広げた後の残差をさらに削る要因であり、最初の主因ではない。
- `server last OutPayload p99` が total p99 に近いため、残りの tail は「server が最後の payload を transport に渡し終えるまで」と強く結びついている。
- ただしこの server timing は client receive window / WINDOW_UPDATE によっても動くため、純粋な server-only bottleneck ではなく、server send と client receive flow-control の相互作用として扱うべき。

## php-grpc-lite / ext-grpc streaming diagnostic

default server 条件:

| case | implementation | msg/s | p50 | p99 | server first OutPayload p99 | server last OutPayload p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 10×100KiB | php-grpc-lite | 8234.9 | 911.8μs | 3771.9μs | 1361.2μs | 3194.3μs |
| 10×100KiB | ext-grpc | 11179.6 | 634.5μs | 3164.9μs | 910.5μs | 2778.2μs |
| 1×1MiB | php-grpc-lite | 767.1 | 922.8μs | 4947.8μs | 4248.8μs | 4248.8μs |
| 1×1MiB | ext-grpc | 1286.0 | 517.6μs | 3674.1μs | 3142.8μs | 3142.8μs |

判断:

- ext-grpc は total latency だけでなく server `OutPayload` p99 も低い。これは server 条件が違うというより、client の receive-side behavior が server send progress に影響していることを示す。
- 1×1MiB では php-grpc-lite と ext-grpc の total p99 差は約 1.27ms、server last OutPayload p99 差は約 1.11ms。差分の大半は client が response を読む前後の flow-control / read progression と結びついている。
- response body append / protobuf decode のような PHP userland 後段だけでは、この差を説明できない。

## 結論

候補は以下のように切れる。

| 候補 | 判断 |
| --- | --- |
| receive loop CPU | 主因ではない。`recv()` / `nghttp2_session_mem_recv()` / send-after-recv の p99 は total tail より十分小さい。 |
| poll scheduling | 単独の処理 cost ではない。p99 と一致するが、実態は DATA / trailers 到着待ち。 |
| receive-side flow-control | 主因の一部。client receive window を広げると WINDOW_UPDATE がほぼ消え、p50/p99 が改善する。 |
| server send / write buffer | 残差に効く。ただし client receive flow-control と相互作用するため、server-only 問題とは扱わない。 |
| ext-grpc 差分 | ext-grpc は receive-side flow-control / read progression が良く、server send progress も早い。native transport では C-core と同様に広い receive window と積極的な read loop を採るべき。 |

## 実装判断

server streaming large response の native transport では、libcurl を外すだけでは不足する。

最低限必要なのは以下。

1. receive stream / connection window を明示的に大きくする。
2. DATA 受信後に WINDOW_UPDATE を遅らせず flush する。
3. response body は chunk list / streaming decode に流し、巨大 body を一括で保持する前提を避ける。
4. poll loop は CPU最適化対象ではなく、read readiness を逃さない設計対象として扱う。

large request unary と同じく nghttp2 を直接使う方針は有効だが、server streaming large response では upload path ではなく receive-side flow-control が設計上の要点になる。
