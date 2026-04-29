# return-transfer fast path reference 2026-04-30

`CURLOPT_WRITEFUNCTION` を外し、unary response body を `curl_exec()` の戻り値で受ける参考計測。通常実装には採用しない前提で、large response の client stack 差分を見るために opt-in diagnostic として追加した。

## 目的

libcurl / PHP ext/curl 経路では、HTTP/2 DATA chunk ごとに PHP ext/curl が `ZVAL_STRINGL()` で PHP string を作り、`zend_call_known_fcc()` で `CURLOPT_WRITEFUNCTION` の user callback を呼ぶ。この cost は `curl_exec()` 内に含まれるが、php-grpc-lite の `body_append_ns_total` には含まれない。

`return-transfer fast path` はこの callback trampoline を外した場合の参考値を取るためのもの。gRPC trailer 互換性にリスクがあるため、採用候補ではなく観測線として扱う。

## 実装

| 変更 | 内容 |
|---|---|
| `src/Grpc/UnaryCall.php` | `php_grpc_lite.return_transfer_fast_path` が true の時だけ `CURLOPT_WRITEFUNCTION` を設定しない |
| `tools/phase2/payload-unary.php` | `--return-transfer-fast-path` を追加 |
| `bench/phase2/run.sh` | `payload-unary-return-transfer-fast-path` suite を追加 |

通常 path は従来通り `CURLOPT_WRITEFUNCTION => $this->onBodyChunk(...)` を使う。fast path は `CURLOPT_RETURNTRANSFER` の戻り値を `returnedBody` として保存し、既存の frame parse / deserialize に渡す。

## 注意点

fast path では body callback が走らないため、現在の「body を受けた後の header は trailers」という判定が使えない。`grpc-status` / `grpc-message` / `grpc-status-details-bin` は status header として trailing metadata に入るが、custom trailing metadata が `grpc-status` より前に来る場合は initial metadata と誤判定する可能性がある。

そのため、この経路は互換実装ではない。採用するなら HTTP/2 trailer block を PHP curl API から安全に復元する別設計が必要になる。

## 参考計測

条件:

| 項目 | 内容 |
|---|---|
| 実行日 | 2026-04-30 |
| 対向 | Go test-server cached payload |
| 実行入口 | `./bench/phase2/run.sh payload-unary-diagnostic-cached` / `payload-unary-return-transfer-fast-path` |
| tag | `phase2-return-transfer-reference-20260430` |
| payload | 100KB / 1MB |
| sample | `--duration=5 --max-calls=1000 --warmup-calls=10` |
| 保存先 | `var/bench-results/phase2-payload-unary-*-phase2-return-transfer-reference-20260430-php-grpc-lite.json` |

| payload | path | calls/s | p50 | p99 | curl exec p99 | starttransfer p99 | download-after-starttransfer p99 | after-curl userland p99 |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 100KB | normal | 5314.9 | 120.1μs | 1976.8μs | 1838.5μs | 1481.0μs | 163.0μs | 19.3μs |
| 100KB | return-transfer | 4276.2 | 151.5μs | 2246.5μs | 2230.0μs | 1306.0μs | 817.0μs | 21.5μs |
| 1MB | normal | 1251.7 | 582.8μs | 4289.3μs | 4105.9μs | 3476.0μs | 2172.0μs | 240.1μs |
| 1MB | return-transfer | 1443.2 | 518.7μs | 3524.6μs | 3475.4μs | 2600.0μs | 2017.0μs | 87.2μs |

| payload | path | body chunks p99 | body append p99 | body assemble p99 | payload slice p99 | deserialize p99 | returned body p50 |
|---:|---|---:|---:|---:|---:|---:|---:|
| 100KB | normal | 13 | 2.6μs | 5.6μs | 7.6μs | 8.4μs | - |
| 100KB | return-transfer | - | - | - | 11.7μs | 11.8μs | 102409B |
| 1MB | normal | 130 | 13.5μs | 74.9μs | 130.6μs | 26.3μs | - |
| 1MB | return-transfer | - | - | - | 47.8μs | 26.9μs | 1048585B |

## 観察

- 100KB は fast path が悪化した。`download-after-starttransfer` p99 が 163.0μs から 817.0μs に増えており、PHP callback を外すことが常に得になるわけではない。
- 1MB は fast path が改善した。p50 は 582.8μs から 518.7μs、p99 は 4289.3μs から 3524.6μs、throughput は 1251.7 calls/s から 1443.2 calls/s に改善した。
- 1MB では after-curl userland p99 が 240.1μs から 87.2μs に下がった。`implode()` と payload `substr()` 周辺の copy が減った可能性が高い。
- ただし 100KB 悪化と trailer 互換性リスクがあるため、現時点では採用しない。

## thesis/grpc-client 参考

`thesis/grpc-client` は AMPHP HTTP client ベースで、HTTP response body を `ReadableStream` として読み、gRPC frame parser に chunk を push する。

| 箇所 | 内容 |
|---|---|
| `_research/thesis-grpc/packages/client/src/Client/Internal/Http2/ConcurrentClientStream.php:72` | trailers は `Response::getTrailers()` から明示的に取得する |
| `_research/thesis-grpc/packages/grpc/src/Internal/Http2/StreamCodec.php:105` | response body stream から chunk を read し parser に渡す |
| `_research/thesis-grpc/packages/grpc/src/Internal/Protocol/Parser.php:35` | parser は `$this->buffer .= $data` で gRPC frame を assemble する |

重要なのは、HTTP client API が trailers を first-class に持っている点。php-curl の header callback では trailer 種別が落ちるため、同じ設計を安全に再現できない。

## swoole/grpc 参考

`swoole/grpc` は Swoole HTTP/2 client を使い、Swoole の response object を stream id ごとの channel で受ける。

| 箇所 | 内容 |
|---|---|
| `_research/swoole-grpc/src/Grpc/Client.php:146` | background coroutine が `$this->client->recv(-1)` で HTTP/2 response を受ける |
| `_research/swoole-grpc/src/Grpc/Client.php:157` | stream id ごとの channel に response を push する |
| `_research/swoole-grpc/src/Grpc/Parser.php:87` | `grpc-status` は `$response->headers` から読む |
| `_research/swoole-grpc/src/Grpc/Parser.php:91` | unary response body は `$response->data` として complete body を扱う |

Swoole 版は libcurl callback ではなく HTTP/2 client object の response abstraction に乗っている。complete body を扱う点は fast path に近いが、trailer/header の扱いは Swoole API 依存で、php-curl の制約とは違う。

## 判断

- `CURLOPT_WRITEFUNCTION` を外す fast path は、1MB large response の参考値では有効だった。
- 100KB では悪化したため、一般化はできない。
- thesis/swoole の参考実装はいずれも HTTP/2 client API が body stream / complete body / trailers を libcurl より高水準に扱う。php-curl だけで同等の互換性を保つのは難しい。
- drop-in replacement の通常実装としては、現行の `WRITEFUNCTION` path を維持する。
- 今後やるなら、fast path は「large unary response かつ trailer 互換性を許容できる diagnostic」としてだけ使い、採用判断には custom trailer / binary metadata の互換テストを先に作る。
