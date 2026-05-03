# nghttp2 PoC: server streaming direct response payload assembly (2026-05-03)

## 目的

`1×1MiB` server streaming では、response buffer compactだけでは受信途中の最大bufferを下げられない。1messageがcompleteするまでconsumed bytesが存在しないため、append-only bufferをcompactしても効果が限定される。

そこで、large single message向けに以下を分解した。

1. PHP callbackに渡すpayload string生成コスト。
2. protobuf decode / PHP yieldコスト。
3. DATAを一度body bufferへappendしてからpayload stringへ再copyする構造の影響。

## 実装

`bench.php` に `--response-callback-mode` を追加した。

- `none`: callbackなし。
- `noop`: payloadをPHP string化してcallbackするが、callback本体は何もしない。
- `strlen`: payload stringを読むだけ。
- `decode`: `BenchReply::mergeFromString()` だけ。
- `decode-yield`: protobuf decode後、generatorで1回yieldする。

`nghttp2_poc` には以下の計測を追加した。

- `call_response_payload_string_us`: callbackへ渡すpayload string生成時間。
- `call_max_response_payload_string_us`: 1messageあたりpayload string生成時間の最大値。

さらに `--direct-response-payload` を追加した。これはincremental decode時に、DATAを汎用body bufferへappendせず、gRPC frame headerを読み取った時点でpayload長ぴったりの `zend_string` を確保し、DATA chunkをその最終payload stringへ直接copyする。completeしたらその `zend_string` をPHP callbackへ渡す。

これにより、従来の以下の二段階copyを避ける。

```text
DATA -> client.body -> PHP payload string
```

新経路は以下。

```text
DATA -> PHP payload string
```

## 1×1MiB 比較

固定条件:

- `server-stream`
- `message-count=1`
- `response-bytes=1048576`
- `iterations=1000`
- `receive window=16MiB`
- `receive buffer=64KiB`
- `--poll-loop`
- `--no-copy`
- `--flush-after-mem-recv`
- `--incremental-decode`

| mode | path | p50 | p99 | msg/s | server last p99 | p99 - server last p99 | append p99 | payload build p99 | callback p99 | max body buffer p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| noop | append | 479.0μs | 4764.0μs | 1196.1 | 4043.6μs | 720.4μs | 44.0μs | 32.0μs | 1.0μs | 1048585B |
| noop | compact | 481.0μs | 4076.0μs | 1285.5 | 3471.3μs | 604.7μs | 44.0μs | 29.0μs | 1.0μs | 1048585B |
| noop | direct | 427.0μs | 3623.0μs | 1447.4 | 3187.9μs | 435.1μs | 0.0μs | 38.0μs | 1.0μs | 0B |
| decode-yield | append | 480.0μs | 3945.0μs | 1318.1 | 3498.9μs | 446.1μs | 40.0μs | 30.0μs | 29.0μs | 1048585B |
| decode-yield | compact | 461.0μs | 3644.0μs | 1332.7 | 3268.7μs | 375.3μs | 40.0μs | 29.0μs | 31.0μs | 1048585B |
| decode-yield | direct | 455.0μs | 3761.0μs | 1379.3 | 3467.5μs | 293.5μs | 0.0μs | 34.0μs | 32.0μs | 0B |

## 100×100KiB 比較

long stream側の比較として `100×100KiB` も再取得した。

| mode | path | p50 | p99 | msg/s | server last p99 | p99 - server last p99 | append p99 | payload build p99 | callback p99 | max body buffer p99 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| noop | append | 5567.0μs | 13650.0μs | 16653.6 | 13277.2μs | 372.8μs | 2892.0μs | 513.0μs | 6.0μs | 10240900B |
| noop | compact | 5588.0μs | 11978.0μs | 16866.8 | 11870.4μs | 107.6μs | 379.0μs | 290.0μs | 8.0μs | 102409B |
| noop | direct | 5448.0μs | 11301.0μs | 17349.8 | 11203.7μs | 97.3μs | 0.0μs | 401.0μs | 8.0μs | 0B |
| decode-yield | append | 5105.0μs | 12162.0μs | 17490.8 | 11888.9μs | 273.1μs | 2553.0μs | 289.0μs | 630.0μs | 10240900B |
| decode-yield | compact | 5880.0μs | 12023.0μs | 16071.5 | 11564.8μs | 458.2μs | 365.0μs | 313.0μs | 352.0μs | 102409B |
| decode-yield | direct | 5706.0μs | 12301.0μs | 16351.3 | 12225.3μs | 75.7μs | 0.0μs | 364.0μs | 341.0μs | 0B |

## 判断

`1×1MiB` ではdirect payload assemblyが有効だった。

- `decode-yield` の p50 は `480.0μs -> 455.0μs`。
- `decode-yield` の p99 は `3945.0μs -> 3761.0μs`。
- `p99 - server last p99` は `446.1μs -> 293.5μs`。
- body buffer保持は `1048585B -> 0B`。

一方、payload string生成そのものは p99で30μs台、protobuf decode/yieldも30μs台だった。したがって `1×1MiB` の主因はPHP decode/yieldではなく、transport受信からapplication payloadへ渡すまでのbuffer/copy構造とserver/transport pacingの混合と見るべき。

`100×100KiB` ではdirectはappend p99を0にできるが、decode-yield込みのwall p99ではcompactと大差ない。long streamでは64KiB compactでも十分に効くため、directはlarge single message向けの追加最適化として扱うのが妥当。

## 実装方針への反映

C拡張実装に向けたresponse path候補は以下。

1. server streamingの基本形はappend-onlyにしない。consumed bytesを解放するcompact/ring bufferを持つ。
2. large single messageを重視するなら、gRPC frame headerを読んだ時点でpayload長ぴったりのbufferを確保し、DATAから最終payload bufferへ直接組み立てる。
3. PHP protobufへ渡す以上、最終的なPHP stringは必要。最適化対象は「PHP stringを作ること」ではなく、その前段でbody bufferを別に作って二重copyすること。
4. `1×1MiB`でext-grpcに残る差は、protobuf decode/yield単体では説明しにくい。次に見るなら、nghttp2 receive loopのpoll/read batching、server last p99との差分、または最終payload bufferを複数DATA chunkから組み立てる際のallocation/copy policy。
