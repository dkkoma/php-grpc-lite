# nghttp2 direct PoC no-copy + poll loop

## 目的

instrumented libcurl / nghttp2 計測で、php-grpc-lite の large request は `libcurl -> nghttp2` の 64KiB body 供給単位と `NGHTTP2_ERR_DEFERRED` / resume ループに主な構造的コストがあると分かった。

このため、nghttp2 direct transport PoC で以下を同時に満たす送信経路を試した。

- gRPC 5B header と protobuf payload を PHP で full concat しない。
- `NGHTTP2_DATA_FLAG_NO_COPY` で nghttp2 内部 DATA buffer への payload copy を避ける。
- no-copy DATA frame の partial write を C 側で状態管理し、nonblocking `poll()` loop と併用する。

## 実装

`poc/nghttp2-client-ext/nghttp2_poc.c` の `send_data_callback()` に、no-copy DATA frame の pending write 状態を追加した。

従来は `NGHTTP2_DATA_FLAG_NO_COPY` の `send_data_callback` が complete DATA frame を blocking `writev()` で書き切る前提だったため、`--poll-loop` と `--no-copy` を併用できなかった。今回、`framehd + grpc header slice + payload slice` の `iovec` と残り bytes を `poc_client` に保持し、`EAGAIN` / `EWOULDBLOCK` では `NGHTTP2_ERR_WOULDBLOCK` を返し、次回 callback で同じ DATA frame の残りから再開するようにした。

これにより、no-copy のまま socket backpressure を poll loop に戻せる。

## 使い方

単体:

```bash
docker compose run --rm dev sh -lc 'cd poc/nghttp2-client-ext && phpize && ./configure --enable-nghttp2-poc && make -j$(nproc)'
docker compose run --rm dev php \
  -d extension=/workspace/poc/nghttp2-client-ext/modules/nghttp2_poc.so \
  poc/nghttp2-client-ext/bench.php \
  --iterations=1000 \
  --response-bytes=100 \
  --request-bytes=1048576 \
  --split-grpc-frame \
  --no-copy \
  --poll-loop
```

php-grpc-lite / ext-grpc / PoC 同一 server 比較:

```bash
BENCH_REQUEST_PAYLOAD_SIZES=1048576 \
BENCH_MAX_CALLS=1000 \
BENCH_POC_ITERATIONS=1000 \
BENCH_POC_POLL_LOOP=1 \
./bench/phase2/compare-request-upload-transports.sh
```

## 結果

### PoC 内比較

同一 dev container / 1MiB request / 100B response / 1000 calls。

| case | calls/s | p50 | p99 | upload complete p50 | upload complete p99 | server InPayload p50 | server InPayload p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| split + no-copy blocking | 1363.4 | 400us | 4539us | 109us | 1321us | 242.2us | 3787.6us |
| split + no-copy + poll loop | 1483.7 | 360us | 3602us | 106us | 693us | 217.4us | 3237.8us |

no-copy + poll loop では `send_wouldblock_calls=1`、`recv_wouldblock_calls=3562`、`poll_calls=2562`。localhost / Docker 条件では send backpressure は少ないが、partial write 状態機械は正常に動作した。

### ext-grpc との同一 server 比較

`BENCH_TAG=nghttp2-poll-upload-20260503`、1MiB request、1000 calls。

| implementation | calls/s | p50 | p99 | server InPayload p50 | server InPayload p99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| php-grpc-lite | 955.1 | 819.9us | 4369.4us | 473.7us | 3872.3us |
| ext-grpc | 1318.8 | 482.8us | 4071.5us | 173.2us | 3831.4us |
| nghttp2 PoC no-copy + poll loop | 1435.3 | 361.0us | 3549.0us | 213.8us | 3331.6us |

この run では PoC が ext-grpc を throughput / p50 / p99 で上回った。ただし PoC は HTTP/2 cleartext、unary large request 専用、API互換なしの研究実装であり、ext-grpc の完全代替性能を示すものではない。

### request size sweep

`BENCH_TAG=nghttp2-nocopy-poll-sweep-20260503`、100B response、1000 calls。

| request bytes | calls/s | p50 | p99 | upload complete p99 | server InPayload p99 | send wouldblock | flow-control pause p99 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 102400 | 4890.5 | 71us | 2560us | 70us | 1692.0us | 0 | 0 |
| 524288 | 2077.5 | 212us | 3263us | 401us | 2928.4us | 1 | 0 |
| 1048576 | 1389.0 | 382us | 3822us | 945us | 3549.5us | 0 | 0 |
| 2097152 | 948.2 | 771us | 4061us | 1562us | 3607.1us | 1 | 0 |

payload size が大きくなると p50 と upload complete p99 は増える。一方、p99 は各 size で server InPayload p99 に寄っており、client-side flow-control pause は p99 でも出ていない。

## 観測

- 新経路では `call_flow_control_pauses_p99=0`。client 側の明示的な flow-control pause は出ていない。
- `client_upload_complete_us_p99=683us` に対して `p99=3549us`、`server_stats_in_payload_ns_p99=3331.6us`。tail は client の DATA write 完了前ではなく、server が request payload を受け切るまでの区間に残る。
- tail sample でも遅い call は `client_upload_complete_us` が 100us 前後で、`client_response_header_us` と `server_stats_in_payload_ns` が伸びる。
- したがって、今回の実装は libcurl 経由の 64KiB provider/resume ループを外す方向として有効。残る p99 は socket write copy よりも、wire / server receive / scheduler を含む区間に寄る。
- 100KB〜2MiB sweep でも同じ傾向で、request size に比例して upload complete は伸びるが、tail の主な観測点は server InPayload 側に残る。

## 判断

大きな詰まりを解消する実装方針として、`nghttp2` を自前で再実装する必要はない。`nghttp2` を直接使い、request body buffer、DATA frame no-copy、partial write、poll loop を C 側 transport が所有する形が最も有望である。

この結果は、Phase 2 の transport 判断として「libcurl を外す価値がある」ことを強める。次に進めるなら、PoC を production extension に寄せる前に、以下を確認する。

- repeated run で改善が安定するか。
- TLS 経路で同じ構造が維持できるか。
- unary API 互換 surface へどう接続するか。
- connection / channel persistent 化した時の lifecycle と error handling。
