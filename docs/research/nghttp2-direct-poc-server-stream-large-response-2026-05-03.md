# nghttp2 direct PoC: server streaming large response (2026-05-03)

## 目的

server streaming large response の p99 tail について、libcurl を外した nghttp2 直接制御 PoC で受信 path を切り分ける。

今回の主な問いは、large response の tail が response DATA を client 側で append / copy する処理に起因するのか、それとも transport / server send / kernel scheduling 側に残るのかである。

## 実装

`poc/nghttp2-client-ext/bench.php` に `--rpc=server-stream` と `--message-count` を追加し、既存の `nghttp2_poc_unary_batch()` を `BenchServerStream` path にも使えるようにした。

`poc/nghttp2-client-ext/nghttp2_poc.c` には以下の受信計測を追加した。

- `--discard-response-body`: response DATA を `smart_str` に append せず破棄する。
- `client_first_response_data_us` / `client_last_response_data_us`: response DATA の初回 / 最終到達時刻。
- `call_response_data_bytes`: 1 call あたりの HTTP/2 DATA payload bytes。
- `call_data_recv_calls`: 1 call あたりの DATA chunk callback 回数。
- `call_body_append_us` / `call_max_body_append_us`: `smart_str_appendl()` の合計 / 最大時間。

この PoC は protobuf decode を行わない。server streaming response を「gRPC framed bytes の受信」として扱うことで、transport 受信と body copy の差を観測する。

## 実行

```bash
docker compose run --rm dev sh -lc 'cd poc/nghttp2-client-ext && make -j$(nproc)'
docker compose run --rm dev sh -lc 'php -d extension=/workspace/poc/nghttp2-client-ext/modules/nghttp2_poc.so /workspace/poc/nghttp2-client-ext/bench.php --rpc=server-stream --iterations=1000 --message-count=10 --response-bytes=102400 --split-grpc-frame --no-copy --poll-loop'
docker compose run --rm dev sh -lc 'php -d extension=/workspace/poc/nghttp2-client-ext/modules/nghttp2_poc.so /workspace/poc/nghttp2-client-ext/bench.php --rpc=server-stream --iterations=1000 --message-count=10 --response-bytes=102400 --split-grpc-frame --no-copy --poll-loop --discard-response-body'
```

比較結果は `var/bench-results/` に保存した。

## 結果

### 10 messages × 100KiB

| implementation | mode | calls/s or msg/s | p50 | p99 | body append p99 | last response DATA p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| nghttp2 PoC | append | 933.7 calls/s | 758.0μs | 4354.0μs | 67.0μs | 4353.0μs |
| nghttp2 PoC | discard | 964.7 calls/s | 723.0μs | 4412.0μs | 0.0μs | 4411.0μs |
| php-grpc-lite | decode | 8514.8 msg/s | 879.4μs | 4119.5μs | - | - |
| ext-grpc | decode | 12927.3 msg/s | 546.8μs | 3029.8μs | - | - |

### 1 message × 1MiB

| implementation | mode | calls/s or msg/s | p50 | p99 | body append p99 | last response DATA p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| nghttp2 PoC | append | 851.9 calls/s | 776.0μs | 5152.0μs | 70.0μs | 5152.0μs |
| nghttp2 PoC | discard | 913.7 calls/s | 702.0μs | 5568.0μs | 0.0μs | 5568.0μs |
| php-grpc-lite | decode | 756.3 msg/s | 954.4μs | 4628.2μs | - | - |
| ext-grpc | decode | 1481.0 msg/s | 475.1μs | 3132.4μs | - | - |

## 判断

server streaming large response の p99 tail は、PoC 上では `smart_str_appendl()` の合計時間では説明できない。

10×100KiB と 1×1MiB の両方で、response body を破棄しても p99 は改善しなかった。`client_last_response_data_us_p99` が total p99 とほぼ一致しており、tail は「client が最後の response DATA を受け取るまで」にある。

したがって、次に見るべき対象は PHP userland の append ではなく、以下である。

1. nghttp2 direct PoC の receive loop / WINDOW_UPDATE 返送 / poll scheduling。
2. Go server の send 側 pacing と HTTP/2 write buffer。
3. libcurl 経路との差ではなく、ext-grpc と nghttp2 PoC の receive-side flow-control / read loop の差。

large request unary と異なり、server streaming large response は「libcurl を外すだけで tail が消える」とはまだ言えない。
