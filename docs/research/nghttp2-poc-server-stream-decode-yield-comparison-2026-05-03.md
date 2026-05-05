# nghttp2 PoC: server streaming decode/yield comparison (2026-05-03)

## 目的

これまでの nghttp2 PoC と ext-grpc の比較は、transport receive path 寄りだった。PoC 側は response DATA の受信、gRPC framed bytes の append/破棄、flow-control を見ていたが、protobuf decode と PHP `Generator` yield 相当の処理を含んでいなかった。

今回は PoC batch 内で response body を gRPC frame parse し、message payload ごとに PHP callable を呼ぶようにした。その callable で `BenchReply::mergeFromString()` と 1-message `Generator` yield を実行し、latency に protobuf decode / PHP callback boundary / yield 相当を含める。

## 実装

`poc/nghttp2-client-ext/nghttp2_poc.c`:

- `nghttp2_poc_unary_batch()` に optional `response_callback` を追加。
- 各 stream 完了後、accumulated response body を gRPC frame として parse。
- 各 message payload を PHP callable に渡し、callback 実行時間を `call_response_decode_us` / `call_max_response_decode_us` に保存。
- callback 有効時は response body discard を無効にする。

`poc/nghttp2-client-ext/bench.php`:

- `--decode-response-messages` を追加。
- callback 内で `Helloworld\BenchReply::mergeFromString()` を実行。
- さらに 1-message generator を通して return し、PHP yield 相当の処理を含める。

## 条件

PoC best 条件:

- server streaming
- receive stream / connection window: 16MiB
- `flush-after-mem-recv`
- receive buffer: 256KiB
- no-copy request path
- poll loop
- protobuf decode + 1-message generator yield

比較対象:

- `ext-grpc`: `tools/phase2/streaming-diagnostic.php`
- `php-grpc-lite`: 同上

## 結果

| case | implementation | throughput | p50 | p99 | server last OutPayload p99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 10×100KiB | nghttp2 PoC decode/yield | 11338.7 msg/s | 550.0μs | 3836.0μs | 3684.9μs |
| 10×100KiB | ext-grpc | 11416.6 msg/s | 627.8μs | 3673.8μs | 3355.8μs |
| 10×100KiB | php-grpc-lite | 8204.7 msg/s | 930.7μs | 4056.6μs | 3170.0μs |
| 1×1MiB | nghttp2 PoC decode/yield | 1308.6 msg/s | 488.0μs | 3823.0μs | 3357.5μs |
| 1×1MiB | ext-grpc | 1236.4 msg/s | 544.6μs | 3916.7μs | 3479.6μs |
| 1×1MiB | php-grpc-lite | 757.8 msg/s | 940.0μs | 4752.8μs | 3754.7μs |

PoC の decode/yield p99:

| case | decoded messages p99 | decode/yield p99 | max per-message decode/yield p99 |
| --- | ---: | ---: | ---: |
| 10×100KiB | 10 | 35.0μs | 11.0μs |
| 1×1MiB | 1 | 34.0μs | 34.0μs |

## 判断

decode/yield を入れても、server streaming PoC は ext-grpc と同じレンジにいる。

- 10×100KiB では、PoC は ext-grpc より throughput はほぼ同等、p50 は低いが、p99 は約 162μs 遅い。
- 1×1MiB では、PoC は ext-grpc より throughput / p50 / p99 のすべてで良い。
- decode/yield p99 は 30〜40μs 程度で、large response p99 の主因ではない。

ただし、PoC の yield は実際の `ServerStreamingCall::responses()` の実装そのものではなく、C extension から PHP callable をmessageごとに呼び、callback内で1-message generatorを通す近似である。本実装時には actual `responses()` surface で再計測する必要がある。

## 結論

前回までの transport-only 比較は完全に公平ではなかったが、protobuf decode / PHP yield 相当を入れても結論は大きく変わらない。

server streaming large response の主な設計要点は引き続き以下。

1. receive stream / connection window を大きくする。
2. `nghttp2_session_mem_recv()` 後に WINDOW_UPDATE / ACK を即 flush する。
3. receive buffer 256KiB は効果があるケースもあるが、default 固定ではなく tuning knob として扱う。

HTTP/2 transport の server streaming は、decode/yield込みでも ext-grpc に近い、または一部ケースで上回る見込みがある。
