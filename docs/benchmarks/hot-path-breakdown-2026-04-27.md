# ホットパス分解と streaming frame split 改善

> **取得日**: 2026-04-27
> **目的**: Channel-scoped curl handle reuse 後に残る header/body callback、metadata 処理、protobuf decode、streaming frame split の CPU コストを分解する

---

## 1. 追加した計測

通常の RPC ベンチは `bench/` + PHPBench のまま維持する。今回の分解はネットワークを通さないローカル CPU 計測なので、通常比較を汚さないように独立スクリプトへ分離した。

```bash
docker compose run --rm dev php tools/bench-hot-path.php
```

---

## 2. 分解結果

| case | avg |
|---|---:|
| request serialize + grpc frame | 83.0 ns |
| response frame parse only 0B | 60.8 ns |
| response frame parse + protobuf merge 0B | 280.4 ns |
| response frame parse only 100B | 65.0 ns |
| response frame parse + protobuf merge 100B | 253.9 ns |
| response frame parse only 1KB | 72.3 ns |
| response frame parse + protobuf merge 1KB | 289.0 ns |
| response frame parse only 10KB | 228.5 ns |
| response frame parse + protobuf merge 10KB | 572.3 ns |
| response frame parse only 100KB | 1.495 μs |
| response frame parse + protobuf merge 100KB | 5.064 μs |
| header/trailer line parse x6 | 581.7 ns |
| stream split current 100 messages | 13.650 μs |
| stream split offset 100 messages | 5.408 μs |
| stream split current + merge 100 messages | 34.175 μs |
| stream split offset + merge 100 messages | 25.347 μs |
| stream split current 1000 messages | 1.239 ms |
| stream split offset 1000 messages | 54.422 μs |
| stream split current + merge 1000 messages | 1.524 ms |
| stream split offset + merge 1000 messages | 261.454 μs |

---

## 3. 実装判断

- unary の request framing、response frame parse、header/trailer parse はサブ μs で、warm unary 35–40 μs の主因ではない。
- protobuf merge は 100KB でも約 5 μs で、100KB unary の 200 μs 台全体から見ると一部に留まる。
- streaming の現行 frame split は、complete frame ごとに `$buffer = substr($buffer, consumed)` していたため、複数 message が 1 chunk にまとまるケースでコピー量が増える。
- `ServerStreamingCall` は buffer offset 方式に変更し、chunk 内の frame を走査してから消費済み prefix を一度だけ捨てるようにした。
- pending queue も `array_shift()` をやめ、offset で読み進める方式にした。

---

## 4. RPC ベンチでの確認

```bash
docker compose run --rm dev vendor/bin/phpbench run bench/ServerStreamingBench.php --report=aggregate
```

| シナリオ | connection-reuse 記録値 | 今回 |
|---|---:|---:|
| count=10 | 245 μs | 240.949 μs |
| count=100 | 378 μs | 349.923 μs |
| count=1000 | 1.40 ms | 1.357 ms |
| payload=0B, count=100 | 333 μs | 303.036 μs |
| payload=10KB, count=100 | 1.13 ms | 1.316 ms |
| delay=10ms, count=10 | 102.0 ms | 103.356 ms |

microbench では frame split の差が大きいが、実 RPC では libcurl callback、HTTP/2 受信、Generator yield、protobuf decode、ベンチ環境の揺れが支配的で、改善幅は小さい。とはいえ、O(n) 的な buffer copy を消せたため、長い stream や chunk に複数 message がまとまるケースへの安全側の変更として妥当。

---

## 5. 次に見る対象

- warm unary の 35–40 μs に残る `curl_exec` / callback dispatch / PHP object construction の内訳。
- server streaming は frame split より、`curl_multi_exec` ループ、`curl_multi_select`、Generator yield、protobuf object 生成の寄与を分ける。
