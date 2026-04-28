# Phase 2 decision comparison 2026-04-29

Phase 2 の最適化判断に使う長めの比較 run。`bench/phase2/preset.sh decision` で取得した。

## 条件

| 項目 | 内容 |
|---|---|
| 実行日 | 2026-04-29 |
| 実行入口 | `bench/phase2/preset.sh decision` |
| 保存 tag | `phase2-decision-20260429` |
| 対向 | Go test-server |
| 比較対象 | `php-grpc-lite` / 公式 `ext-grpc` |
| 保存先 | `var/bench-results/phase2-*-phase2-decision-20260429-*.json` |

生成 JSON は Phase 2 result contract で検証済み。

## Unary

| suite | php-grpc-lite | ext-grpc | 観察 |
|---|---:|---:|---|
| `throughput-unary` | 25905.2 calls/s | 17237.7 calls/s | 軽量 unary は php-grpc-lite が高 throughput |
| `payload-unary 100KB` | 5442.4 calls/s | 6043.2 calls/s | 100KB は ext-grpc が throughput 優位 |
| `payload-unary 100KB p99` | 2310.4μs | 1341.9μs | 100KB tail は php-grpc-lite が悪い |

100KB 以外の payload では php-grpc-lite の p50/p99 と throughput は良好。大きい response body だけ tail が残る。

## Payload diagnostics

`payload-breakdown` はネットワークを外した hot path。100KB でも slice/decode は μs 台だった。

| operation | p99 |
|---|---:|
| 100KB payload slice | 2.0μs |
| 100KB decode only | 6.2μs |
| 100KB slice + decode | 6.2μs |
| 100KB deserialize apply | 5.8μs |

`payload-unary-diagnostic` は実 RPC 内の opt-in diagnostic。

| metric | p50 | p99 |
|---|---:|---:|
| total unary latency | 94.1μs | 2226.2μs |
| `curl_exec` | 84.2μs | 2187.4μs |
| curl total time | 82.0μs | 2186.0μs |
| curl pretransfer time | - | 108.0μs |
| curl starttransfer time | - | 1894.0μs |
| body append total | - | 9.0μs |
| payload slice | - | 4.6μs |
| deserialize | - | 6.2μs |

100KB tail は `curl_exec` / curl total time 内に集中している。body append、frame parse、payload slice、protobuf deserialize は tail の主因ではない。

追加で `x-bench-server-timing: 1` を送った場合のみ、Go test-server が handler duration と payload allocation duration を trailers に載せるようにした。これにより `curl starttransfer` の内訳として、server handler 側の tail も観測できる。

```bash
BENCH_TAG=phase2-server-timing-20260429 ./bench/phase2/run.sh payload-unary-diagnostic --duration=3 --payload-sizes=102400 --warmup-calls=10
```

| metric | p50 | p99 |
|---|---:|---:|
| total unary latency | 93.0μs | 2028.1μs |
| `curl_exec` | 83.0μs | 1960.5μs |
| curl starttransfer | 37.0μs | 1684.0μs |
| server handler | 2.4μs | 585.3μs |
| server payload allocation | 0.9μs | 563.6μs |
| body append total | - | 9.4μs |
| payload slice | - | 5.0μs |
| deserialize | - | 7.5μs |

この run では `curl starttransfer` p99 の一部は Go test-server 側の 100KB payload allocation tail で説明できる。ただし `starttransfer` p99 1684μs に対して server handler p99 585μs なので、残りは gRPC-Go marshal / HTTP/2 write、libcurl receive、または Docker scheduler の範囲に残る。

server payload allocation を外すため、`x-bench-server-cached-payload: 1` で test-server の事前生成 payload を返す診断も追加した。

```bash
BENCH_TAG=phase2-server-cached-20260429 ./bench/phase2/run.sh payload-unary-diagnostic --duration=3 --payload-sizes=102400 --warmup-calls=10
BENCH_TAG=phase2-server-cached-20260429 ./bench/phase2/run.sh payload-unary-diagnostic-cached --duration=3 --payload-sizes=102400 --warmup-calls=10
```

| metric | normal p99 | cached p99 |
|---|---:|---:|
| calls/sec | 5413.0 | 6877.7 |
| total unary latency | 2157.9μs | 1605.4μs |
| `curl_exec` | 2132.3μs | 1497.6μs |
| curl starttransfer | 1882.0μs | 1106.0μs |
| server handler | 827.2μs | 13.7μs |
| server payload allocation | 682.9μs | 2.2μs |
| body append total | 7.5μs | 9.5μs |
| deserialize | 6.5μs | 7.9μs |

cached payload では throughput が上がり、p99 tail も大きく下がる。100KB unary の悪化は client decode/copy ではなく、ベンチ server が毎回 100KB payload を allocate する条件に強く影響されていた。cached 条件でも `curl starttransfer` p99 は 1ms 程度残るため、残りは gRPC-Go marshal / HTTP/2 write / Docker scheduler / libcurl receive の複合として扱う。

## RTT

| scenario | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc p50 | ext-grpc p99 |
|---|---:|---:|---:|---:|
| warm direct | 39.5μs | 87.0μs | 66.2μs | 274.1μs |
| cold direct | 205.7μs | 754.5μs | 72.6μs | 582.1μs |
| warm 1ms | 1978.2μs | 2196.4μs | 1954.4μs | 2977.2μs |
| cold 1ms | 2007.8μs | 3965.0μs | 1968.6μs | 2083.9μs |
| warm 3ms | 4782.1μs | 5331.8μs | 4693.9μs | 5615.4μs |
| cold 3ms | 6225.2μs | 8793.5μs | 4939.2μs | 5650.9μs |
| warm 5ms | 6934.6μs | 7978.3μs | 7082.4μs | 7844.1μs |
| cold 5ms | 7824.2μs | 10505.7μs | 7011.7μs | 7612.2μs |

direct warm は php-grpc-lite が軽い。cold と RTT ありでは php-grpc-lite の p99 が悪化しやすく、connection reuse / cold path の観測価値が高い。

## Streaming

| suite | php-grpc-lite | ext-grpc | 観察 |
|---|---:|---:|---|
| `throughput-streaming` | 725744.2 msg/s | 383759.0 msg/s | decision 条件では php-grpc-lite が高 throughput |
| `large-streaming 10K` | 790147.2 msg/s | 344685.2 msg/s | php-grpc-lite 優位 |
| `large-streaming 100K` | 1366440.5 msg/s | 425908.1 msg/s | php-grpc-lite 優位 |
| `payload-streaming 10KB` | 72076.9 msg/s | 76898.9 msg/s | 10KB streaming は ext-grpc が僅かに優位 |

streaming は全体として php-grpc-lite が強い。payload が大きい streaming は差が縮む。

## Metadata

| scenario | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc p50 | ext-grpc p99 |
|---|---:|---:|---:|---:|
| req 0 / resp 0 | 44.5μs | 135.5μs | 61.2μs | 144.3μs |
| req 50 / resp 0 | 118.0μs | 464.9μs | 94.7μs | 465.4μs |
| req 50 / resp 50 | 287.1μs | 736.3μs | 227.7μs | 667.6μs |

metadata が多いケースでは ext-grpc の p50 が優位。header parse / metadata append は将来の改善候補だが、100KB payload tail より優先度は低い。

## 判断

| 対象 | 判断 |
|---|---|
| curl handle / connection reuse | 接続確立は主因ではなさそう。reuse 済み前提で cold / RTT の観測線として扱う |
| payload decode / copy | 現状の主犯ではない。C 化候補としての優先度は下げる |
| Go test-server payload allocation | 100KB tail の大きな部分を説明する。client 実装改善対象ではなく、ベンチ解釈上の注意点 |
| streaming hot path | 現状は大きな弱点ではない。改善より回帰監視を優先 |
| metadata path | 多 metadata で固定費は見える。payload tail と reuse の後に見る |

次は通常の `payload-unary` と diagnostic の解釈を分ける。client 側の改善候補としては 100KB unary tail より、cold / RTT と metadata の固定費を見る方が妥当。
