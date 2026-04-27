# Metadata volume benchmark

> 実行日: 2026-04-27
> 対象: `php-grpc-lite` / 公式 `ext-grpc`
> コマンド: `./bench/run.sh metadata`

## 1. 目的

request metadata、initial metadata、trailing metadata の key 数が増えたときの unary 固定費を観測する。binary metadata は互換性確認が未完了のため、この bench では ASCII metadata のみを扱う。

Go test-server の `BenchUnary` は request metadata の制御ヘッダを読み、指定数の `x-bench-initial-*` と `x-bench-trailing-*` を返す。PHPBench 側では `wait()` 後に `getMetadata()` / `getTrailingMetadata()` を呼び、期待した response metadata 数が見えていることも確認する。

## 2. 結果

| set | php-grpc-lite | ext-grpc | 観測 |
|---|---:|---:|---|
| `req_0_resp_0` | **39.027 us** | 74.531 us | 軽量 unary baseline は php-grpc-lite が速い |
| `req_10_resp_0_value_32b` | **53.652 us** | 85.991 us | request metadata 10 keys でも php-grpc-lite が速い |
| `req_50_resp_0_value_32b` | **112.478 us** | 113.380 us | request metadata 50 keys ではほぼ同等 |
| `req_10_resp_10_value_32b` | **62.735 us** | 93.134 us | response metadata 10 pairs では php-grpc-lite が速い |
| `req_50_resp_50_value_32b` | 232.833 us | **210.989 us** | response metadata 50 pairs まで増やすと ext-grpc が速い |

| 実装 | mem_peak |
|---|---:|
| php-grpc-lite | 1.944 mb |
| ext-grpc | 1.038 mb |

## 3. 判断

- request metadata だけなら 50 keys までは ext-grpc とほぼ同等までに収まる。
- response metadata が 50 initial + 50 trailing まで増えると、PHP 側の header line parse と array 構築の固定費が見える。
- mem_peak は他の bench と同じ傾向で、php-grpc-lite は ext-grpc より約 0.9 mb 大きい。
- binary metadata は性能 bench に混ぜず、まず `docs/compatibility-control-checklist.md` の metadata compatibility として ext-grpc 互換を確認する。

## 4. 生成物

- `var/bench-results/metadata-20260427-214249-php-grpc-lite.log`
- `var/bench-results/metadata-20260427-214249-php-grpc-lite.json`
- `var/bench-results/metadata-20260427-214249-php-grpc-lite.tsv`
- `var/bench-results/metadata-20260427-214249-ext-grpc.log`
- `var/bench-results/metadata-20260427-214249-ext-grpc.json`
- `var/bench-results/metadata-20260427-214249-ext-grpc.tsv`
