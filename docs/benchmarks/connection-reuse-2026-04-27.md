# Channel-scoped curl handle reuse 観測

> **取得日**: 2026-04-27
> **対象**: `Channel` が libcurl easy handle を保持し、正常完了した call で `curl_reset()` 後に再利用する実装
> **目的**: ext-grpc に近づけることではなく、libcurl / HTTP/2 connection reuse が純 PHP 実装の固定費にどれだけ効くかを観測する

---

## 1. 実装条件

- `Channel` が idle easy handle を保持する。
- `UnaryCall::wait()` は正常完了時に handle を Channel へ返す。
- `ServerStreamingCall::responses()` は最後まで完走し、かつ curl error が無い場合だけ handle を Channel へ返す。
- curl error、`cancel()`、server streaming の途中終了は接続状態が不明なため handle を破棄する。
- handle を返す前に `curl_reset()` し、前回 call の header/body callback や request options を消す。

---

## 2. 比較結果

同じ `./bench/compare.sh`、同じ Go test-server、同じ PHPBench 設定で計測した。

### 2.1 Unary

| シナリオ | Phase 0 php-grpc-lite | reuse 後 php-grpc-lite | ext-grpc | 観測 |
|---|---:|---:|---:|---|
| `UnaryLatencyBench` / `SayHello` | 228 μs | **37.9 μs** | 68.1 μs | reuse 後は ext-grpc より速い |
| `UnaryPayloadBench` payload=0 | 229 μs | **36.3 μs** | 66.9 μs | 固定費がほぼ消えた |
| payload=100 B | 221 μs | **36.0 μs** | 71.3 μs | payload 小では PHP 側が優位 |
| payload=1 KB | 243 μs | **37.5 μs** | 69.3 μs | decode より固定費支配 |
| payload=10 KB | 250 μs | **44.5 μs** | 73.0 μs | まだ PHP 側が優位 |
| payload=100 KB | 384 μs | **195 μs** | 229 μs | per-byte 支配でも僅かに優位 |

Phase 0 で見えていた ~150-190 μs/call の差は、接続確立・HTTP/2 negotiation を毎回払っていたことが主因だった。easy handle reuse 後は軽量 unary が 38 μs 前後まで下がり、C-core の ext-grpc より低い値になった。

### 2.2 Unary with server delay

| シナリオ | reuse 後 php-grpc-lite | ext-grpc | 観測 |
|---|---:|---:|---|
| single, server 10 ms | **11.6 ms** | 11.8 ms | ほぼ同等 |
| 10 calls 逐次, 各 10 ms | **117.5 ms** | 118.7 ms | ほぼ同等 |

server 側 10ms delay が入ると固定費差は計測上ほぼ消える。Phase 0 の 17.2 ms / 174 ms からの低下は、各 call の接続確立が消えた効果として説明できる。

### 2.3 Server streaming

| シナリオ | reuse 後 php-grpc-lite | ext-grpc | 観測 |
|---|---:|---:|---|
| count=10, payload=100 B | 245 μs | **120 μs** | 小件数は ext-grpc 優位 |
| count=100, payload=100 B | 378 μs | **348 μs** | ほぼ同等 |
| count=1000, payload=100 B | **1.40 ms** | 2.63 ms | PHP 側の per-message 優位は維持 |
| count=100, payload=10 KB | **1.13 ms** | 1.23 ms | 今回 run では PHP 側が僅かに優位 |
| delay=10 ms, count=10 | 102.0 ms | **101.1 ms** | server pacing 支配で同等 |

server streaming は既に 1 call あたり 1 stream で、複数 message が同一 HTTP/2 stream 内を流れる。今回の reuse は主に call 開始時の固定費に効くため、message count が多いケースでは既存の per-message 傾向がそのまま残る。

---

## 3. 判断

- Channel lifetime に libcurl connection cache を寄せるだけで、pure PHP の unary 固定費は ext-grpc 比較で十分低い水準まで下がった。
- ext-grpc の数値に近づけること自体は目的ではないが、「Phase 0 の unary 差は C 実装差ではなく per-call connection setup が支配的」という仮説は支持された。
- 次に見るべき対象は、接続再利用後にも残る header/body callback、metadata 処理、protobuf decode のホットパス。

---

## 4. 検証

```bash
docker compose run --rm dev vendor/bin/phpunit
./bench/compare.sh
```

- PHPUnit: 17 tests / 78 assertions OK
- PHPBench: php-grpc-lite / ext-grpc 両環境で完走
