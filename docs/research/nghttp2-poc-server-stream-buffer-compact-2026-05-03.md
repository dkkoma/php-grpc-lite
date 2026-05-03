# nghttp2 PoC: server streaming response buffer compaction (2026-05-03)

## 目的

server streaming large response / long stream で、逐次 decode/yield 済みの response body を保持し続ける append-only 構造が wall time に効いているかを確認する。

前回の逐次 decode/yield PoCでは、complete gRPC frame を DATA 受信中に decode していたが、`client.body` 自体は受信済みbyteを蓄積し続けていた。今回は `--compact-response-buffer` を追加し、decode済みの consumed bytes を `memmove()` で捨て、次の frame 探索を先頭から再開できるようにした。

## 実装

PoC に以下を追加した。

- `--compact-response-buffer`
- `--response-compact-threshold`
- `call_body_compact_count`
- `call_body_compact_bytes`
- `call_body_compact_us`
- `call_max_body_compact_us`
- `call_max_body_buffer_bytes`

compact は `--incremental-decode` 時だけ有効にする。complete gRPC frame を decode/yield したあと、`response_parse_offset >= response_compact_threshold` なら消費済み領域を捨てる。

## 短いstreamでの比較

固定条件:

- `--poll-loop`
- `--no-copy`
- `--flush-after-mem-recv`
- `--decode-response-messages`
- `--incremental-decode`

| case | pattern | window | buffer | threshold | p50 | p99 | msg/s | server last p99 | append p99 | compact p99 | compact count p99 | max body buffer p99 | decode/yield p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1000×100B | append | 8MiB | 32KiB | - | 4467.0μs | 8375.0μs | 215383.1 | 8315.7μs | 22.0μs | 0.0μs | 0 | 107000B | 429.0μs |
| 1000×100B | compact_1 | 8MiB | 32KiB | 1B | 4290.0μs | 7995.0μs | 221608.1 | 7890.2μs | 26.0μs | 19.0μs | 1000 | 107B | 435.0μs |
| 1000×100B | compact_64k | 8MiB | 32KiB | 64KiB | 4311.0μs | 7284.0μs | 223343.9 | 7090.1μs | 25.0μs | 1.0μs | 1 | 65591B | 799.0μs |
| 1000×100B | compact_256k | 8MiB | 32KiB | 256KiB | 4379.0μs | 8364.0μs | 221869.7 | 8292.6μs | 25.0μs | 0.0μs | 0 | 107000B | 329.0μs |
| 10×100KiB | append | 8MiB | 64KiB | - | 497.0μs | 3610.0μs | 12244.0 | 3429.0μs | 43.0μs | 0.0μs | 0 | 1024090B | 55.0μs |
| 10×100KiB | compact_1 | 8MiB | 64KiB | 1B | 489.0μs | 3597.0μs | 12394.0 | 3394.9μs | 32.0μs | 1.0μs | 10 | 102409B | 46.0μs |
| 10×100KiB | compact_64k | 8MiB | 64KiB | 64KiB | 478.0μs | 3368.0μs | 12306.8 | 3196.5μs | 33.0μs | 1.0μs | 10 | 102409B | 42.0μs |
| 10×100KiB | compact_256k | 8MiB | 64KiB | 256KiB | 487.0μs | 4009.0μs | 11889.8 | 3836.4μs | 32.0μs | 1.0μs | 3 | 307227B | 42.0μs |
| 1×1MiB | append | 16MiB | 64KiB | - | 496.0μs | 5058.0μs | 1252.8 | 4249.2μs | 41.0μs | 0.0μs | 0 | 1048585B | 32.0μs |
| 1×1MiB | compact_1 | 16MiB | 64KiB | 1B | 513.0μs | 3960.0μs | 1242.0 | 3271.1μs | 45.0μs | 0.0μs | 1 | 1048585B | 35.0μs |
| 1×1MiB | compact_64k | 16MiB | 64KiB | 64KiB | 507.0μs | 4574.0μs | 1257.1 | 3922.5μs | 62.0μs | 1.0μs | 1 | 1048585B | 37.0μs |
| 1×1MiB | compact_256k | 16MiB | 64KiB | 256KiB | 490.0μs | 4066.0μs | 1265.0 | 3681.7μs | 40.0μs | 0.0μs | 1 | 1048585B | 30.0μs |

短いstreamでは、compactのwall time改善はケースにより揺れる。特に `1×1MiB` は1messageがcompleteするまでdecodeできないため、最大body bufferは1MiBのままで、compactは受信中のbuffer成長を防げない。

## 長いstreamでの比較

append-only構造が効くかを見るため、長いstreamを追加した。

| case | pattern | window | buffer | threshold | p50 | p99 | msg/s | server last p99 | p99 - server last p99 | append p99 | compact p99 | compact count p99 | max body buffer p99 | decode/yield p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10000×100B | append | 8MiB | 32KiB | - | 41784.0μs | 61973.0μs | 233723.6 | 59787.0μs | 2186.0μs | 302.0μs | 0.0μs | 0 | 1070000B | 4566.0μs |
| 10000×100B | compact_1 | 8MiB | 32KiB | 1B | 41902.0μs | 53512.0μs | 237471.9 | 53267.2μs | 244.8μs | 148.0μs | 125.0μs | 10000 | 107B | 3590.0μs |
| 10000×100B | compact_64k | 8MiB | 32KiB | 64KiB | 41955.0μs | 57656.0μs | 231825.9 | 57288.2μs | 367.8μs | 208.0μs | 1.0μs | 16 | 65591B | 3070.0μs |
| 10000×100B | compact_256k | 8MiB | 32KiB | 256KiB | 42679.0μs | 52506.0μs | 231993.3 | 52126.7μs | 379.3μs | 225.0μs | 0.0μs | 4 | 262150B | 3018.0μs |
| 100×100KiB | append | 8MiB | 64KiB | - | 5817.0μs | 12528.0μs | 16647.3 | 12329.8μs | 198.2μs | 2648.0μs | 0.0μs | 0 | 10240900B | 503.0μs |
| 100×100KiB | compact_1 | 8MiB | 64KiB | 1B | 5594.0μs | 11612.0μs | 17143.3 | 11474.3μs | 137.7μs | 330.0μs | 4.0μs | 100 | 102409B | 351.0μs |
| 100×100KiB | compact_64k | 8MiB | 64KiB | 64KiB | 5080.0μs | 9971.0μs | 18136.6 | 9893.8μs | 77.2μs | 259.0μs | 3.0μs | 100 | 102409B | 364.0μs |
| 100×100KiB | compact_256k | 8MiB | 64KiB | 256KiB | 5539.0μs | 11895.0μs | 16677.5 | 11815.6μs | 79.4μs | 317.0μs | 2.0μs | 33 | 307227B | 525.0μs |

## 判断

compact は wall time に効く対象がある。

特に `100×100KiB` では append-only の `call_body_append_us_p99` が 2648μs まで伸び、最大body bufferも約10MiBまで成長した。`compact_64k` では最大body bufferが約100KiBに抑えられ、append p99も259μsまで下がった。これはserver last p99の揺れを差し引いても、クライアント側のappend/保持構造に改善余地があることを示す。

`10000×100B` でも最大body bufferは約1.07MiBからthreshold相当まで落ちる。ただしmany-smallではdecode/yieldとserver pacingの比率が大きく、compact頻度を上げすぎるとcompact自体の回数が増える。`compact_64k` または `compact_256k` のように、ある程度まとめて捨てる方が実装方針としては安全に見える。

`1×1MiB` は1message単体なので、complete frame到着前に消費済み領域が存在しない。compact/ring bufferでは受信途中の最大bufferを下げられず、ここはmessage payloadのcopy/decode pathか、applicationへ渡すpayload表現の問題として別に見る必要がある。

## 次の方針

C拡張実装に入れるなら、server streaming response bufferは append-only ではなく consumed bytes を解放できる構造にする。

現時点の候補:

1. まずはcompact方式。実装が単純で、long streamのappend p99と最大bufferを大きく下げられる。
2. thresholdは固定即時compactではなく、64KiB程度から検討する。many-smallでcompact回数を抑えられ、100KiB級messageではmessageごとに捨てられる。
3. さらに詰めるならring buffer。ただしPHP callbackへ渡すpayloadは現状 `ZVAL_STRINGL` でcopyするため、ring化だけではprotobuf decode入力copyは消えない。

結論として、compact/ring bufferは単なるメモリ削減ではなく、long streamのwall time構造に効く改善候補として扱う。
