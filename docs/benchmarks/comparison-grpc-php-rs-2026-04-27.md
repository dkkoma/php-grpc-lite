# grpc-php-rs one-shot 比較

> **取得日**: 2026-04-27
> **対象**: `php-grpc-lite` / 公式 `ext-grpc` / `BSN4/grpc-php-rs`
> **位置づけ**: 今回限りの観測比較。通常のベンチ比較線は `php-grpc-lite` vs 公式 `ext-grpc` のまま維持する。

---

## 1. 比較条件

- `grpc-php-rs`: <https://github.com/BSN4/grpc-php-rs>
- 利用イメージ: `ghcr.io/bsn4/grpc-php-rs:latest-php8.4`
- `php-grpc-lite`: `docker compose run --rm dev vendor/bin/phpbench run --report=aggregate`
- 公式 `ext-grpc`: `docker compose run --rm dev-ext-grpc bash -c 'cd bench-comparison && vendor/bin/phpbench run --report=aggregate'`
- `grpc-php-rs`: `docker compose run --rm dev-grpc-rs bash -c 'cd bench-comparison && vendor/bin/phpbench run --report=aggregate'`
- 対向サーバは既存の Go test-server。PHPBench のシナリオ、revs、iterations は既存の `bench/` / `bench-comparison/` と同一。

再実行用に任意スクリプトを追加した。

```bash
./bench/compare-rs.sh
```

通常の比較は引き続き以下を使う。

```bash
./bench/compare.sh
```

---

## 2. 結果

| シナリオ | php-grpc-lite | 公式 ext-grpc | grpc-php-rs |
|---|---:|---:|---:|
| cold: client/channel construct + `SayHello` | 212.870 μs | **81.840 μs** | 266.767 μs |
| warm unary: `SayHello` | **35.446 μs** | 69.511 μs | 82.216 μs |
| unary payload 0 B | **35.158 μs** | 71.296 μs | 53.834 μs |
| unary payload 100 B | **36.707 μs** | 69.513 μs | 60.472 μs |
| unary payload 1 KB | **36.012 μs** | 68.779 μs | 55.281 μs |
| unary payload 10 KB | **45.107 μs** | 78.866 μs | 64.516 μs |
| unary payload 100 KB | 251.578 μs | 272.570 μs | **208.238 μs** |
| stream count=10 | 234.263 μs | 141.387 μs | **112.342 μs** |
| stream count=100 | **375.150 μs** | 407.054 μs | 406.885 μs |
| stream count=1000 | **1.376 ms** | 2.843 ms | 11.646 ms |
| stream payload 0 B | **333.062 μs** | 360.400 μs | 413.379 μs |
| stream payload 100 B | **331.022 μs** | 395.202 μs | 408.852 μs |
| stream payload 1 KB | **386.523 μs** | 506.051 μs | 552.509 μs |
| stream payload 10 KB | 1.329 ms | **1.290 ms** | 1.409 ms |
| paced stream delay=0 ms | 233.208 μs | 149.063 μs | **104.249 μs** |
| paced stream delay=1 ms | 17.826 ms | **12.365 ms** | 17.976 ms |
| paced stream delay=10 ms | 103.704 ms | **96.324 ms** | 101.558 ms |
| unary server delay 10 ms | 12.116 ms | **10.730 ms** | 12.130 ms |
| 10 sequential unary, server delay 10 ms | 120.413 ms | **108.710 ms** | 121.870 ms |

---

## 3. 観測

- `grpc-php-rs` は unary payload 100 KB で最速だった。小さい unary では `php-grpc-lite` の Channel-scoped curl handle reuse 後の固定費が最も小さい。
- cold 近似では公式 `ext-grpc` が明確に速い。これは ext-grpc が request をまたぐ C-core 側 pool を使える一方、`php-grpc-lite` と今回の `grpc-php-rs` 比較条件では PHP object lifetime に寄るためと見ている。
- server streaming はシナリオごとの傾向が分かれた。`grpc-php-rs` は count=10 や delay=0 ms では速いが、count=1000 では 11.646 ms まで伸び、per-message 経路に大きな差が出た。
- server 側 delay が支配的なケースは、実装差よりサーバ待ち時間の影響が大きい。ただし今回の run では公式 `ext-grpc` が delay 系で一貫して短い。
- この比較は外部プロジェクトの `latest-php8.4` イメージに依存する one-shot 観測であり、通常の継続比較や最適化判断の基準にはしない。

---

## 4. 検証

```bash
docker compose build dev-grpc-rs
docker compose run --rm dev-grpc-rs php -m | sort | grep -E '^(grpc|protobuf)$'
./bench/compare-rs.sh
```

- `dev-grpc-rs` image build: OK
- `grpc` / `protobuf` extension load: OK
- PHPBench: 3 環境すべて完走
