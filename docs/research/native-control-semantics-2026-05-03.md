# native control semantics progress (2026-05-03)

## 目的

direct / compact のdefault選択ルール以外で、判断なしに進められるnative control系を進めた。

## 実装済み

- native deadline
  - `nghttp2_poc_unary_batch()` に `timeout_us` を追加。
  - poll loopの待ち時間をdeadline残時間で短縮し、期限到達時は `timed_out=true` を返す。
  - PHP wrapperは `STATUS_DEADLINE_EXCEEDED` に変換する。
- native cancel
  - `cancel()` 前に `wait()` / `responses()` した場合は `STATUS_CANCELLED` を返す。
  - server streaming iteration中の `cancel()` は次のresume時に `STATUS_CANCELLED` で止める。
  - 現MVPはbatch drain後にyieldするため、transport-level `RST_STREAM` 送信ではなくAPI surface上のcancel扱いである。
- missing `grpc-status`
  - native resultの `grpc_status < 0` を `STATUS_UNKNOWN` に合成する。
  - HTTP/2 stream reset codeは基本statusへ変換する。
- TLS / mTLS
  - native MVPはh2c onlyのため、SSL credentialsでnativeを明示した場合はcurlへfallbackせず `STATUS_UNAVAILABLE` を返す。
  - TLS / mTLSの実運用安定経路は現時点では `php_grpc_lite.transport=curl`。
- slow consumer limitation check
  - actual HTTP/2 surface用のslow consumer runnerを追加。
  - 現MVP HTTP/2 wrapperはbatch drain後にyieldするため、真のbackpressure検証ではなく制限事項の確認である。

## 検証

```bash
docker compose run --rm dev vendor/bin/phpunit
docker compose run --rm dev php -d extension=/workspace/poc/nghttp2-client-ext/modules/nghttp2_poc.so vendor/bin/phpunit tests/Integration/Http2TransportControlTest.php
BENCH_TAG=20260503-native-slow-consumer-smoke STREAMS=3 MESSAGE_COUNT=20 PAYLOAD_BYTES=100 SLEEP_US=1000 bench/check-native-slow-consumer.sh
docker compose run --rm dev php tools/benchmark/slow-consumer-surface.php --output=var/bench-results/phase2-curl-slow-consumer-20260503-smoke.json --streams=3 --message-count=20 --payload-bytes=100 --sleep-us=1000 --transport=curl
```

結果:

| check | result |
| --- | --- |
| normal PHPUnit | 40 tests / 129 assertions / 3 skipped |
| native extension control PHPUnit | 6 tests / 13 assertions / 1 skipped |
| native slow consumer smoke | first-yield p50 518.3μs, stream p99 37185.4μs |
| curl slow consumer reference | first-yield p50 521.7μs, stream p99 38998.9μs |

slow consumer smokeは短い確認であり、性能判断用ではない。sleep支配の条件ではnative/curlとも概ね同じレンジに見えるが、native MVPは全responseを先にdrainしてからyieldするため、production backpressure要件を満たしたとは判断しない。

## 積み残し

判断なしで進められる実装としては、現MVP surfaceで可能な範囲を一通り入れた。

残りは以下。

- native production streaming resourceを作り、messageごとにtransportからyieldできるようにする。
- production streaming resource上で `cancel()` を `RST_STREAM(CANCEL)` として送る。
- production streaming resource上でslow consumer/backpressureを再検証する。
- TLS / mTLS をnative C transportへ実装するか、native defaultのproduction gateではcurl stable route必須として扱うかを決める。
- direct / compact のdefault選択ルールを決める。
