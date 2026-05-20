---
Status: Open
Owner: Codex
Created: 2026-05-20
Related:
  - docs/issues/open/2026-05-16-ext-grpc-1580-load-search.md
  - docs/issues/open/2026-05-19-compare-official-lite-sa-json-wire-shape.md
---

# Laravel/FPM real Spanner SA JSON CPU差再現

## 目的

以前のLaravel/FPM real Spanner CPU比較はADC条件を含んでおり、SA JSON / JWT条件でofficial ext-grpcだけが速くなる差を見落としていた可能性がある。当時の `bench/fpm-laravel-spanner-load-compare.sh` の方法を使い、credentialをSA JSONにしてCPU/request差が再現するか確認する。

## 方針

- 既存runnerを変更せず、compose overrideでSA JSONをFPM serviceへ渡す。
- ext-grpc側は当時の比較条件に合わせ、`var/official-ext-grpc-so/1.58.0-optimized/grpc.so` をFPM image内のgrpc.soへbind mountする。
- 実Spannerは `vast-falcon-165704` / `bench` / `laravel-bench-db` を使う。
- 最初は当時差が見えた `select_1row_10col` c4 と `transaction_select2_update1_insert1` c16 を優先する。

## スコープ

- Laravel/FPM load runnerによる再現確認。
- CPU/request、RPS、p50/p90/maxの記録。

## 非スコープ

- production codeの変更。
- SA JSONファイルの保存。
- 原因修正。

## 進捗

- [x] SA JSON / ext-grpc 1.58 optimized 用compose overrideをgit管理外に作成。
- [x] `select_1row_10col` c4の比較。
- [x] `transaction_select2_update1_insert1` c16の比較。
- [x] 結果の判断。

## 検証条件

- compose override: `var/fpm-sa-json-ext158.override.yaml`。git管理外。
- credential: service account JSONを `/var/www/sa.json` にread-only mountし、`GOOGLE_APPLICATION_CREDENTIALS=/var/www/sa.json` をFPMへ渡した。
- ext-grpc: `var/official-ext-grpc-so/1.58.0-optimized/grpc.so` をFPM image内のgrpc.soへbind mountした。
- ext-grpc version確認: `COMPOSE_FILE=compose.yaml:var/fpm-sa-json-ext158.override.yaml docker compose exec -T fpm-ext-grpc-16 php --ri grpc` で `grpc module version => 1.58.0`。
- Spanner: `vast-falcon-165704` / `bench` / `laravel-bench-db`。
- worker: FPM 16 workers / CPU quota 4.0。
- runner: `bench/fpm-laravel-spanner-load-compare.sh`。

## 結果

### select_1row_10col c4

Command:

```bash
COMPOSE_FILE=compose.yaml:var/fpm-sa-json-ext158.override.yaml \
LARAVEL_SPANNER_EMULATOR_HOST= \
LARAVEL_SPANNER_PROJECT_ID=vast-falcon-165704 \
LARAVEL_SPANNER_INSTANCE_ID=bench \
LARAVEL_SPANNER_DATABASE_ID=laravel-bench-db \
BENCH_ACTIONS=select_1row_10col \
BENCH_RUN_ID=sa-json-ext158-select-c4-20260520 \
./bench/fpm-laravel-spanner-load-compare.sh 96 4
```

| variant | requests | rps | cpu_us/req | avg_ms | p50_ms | p90_ms | max_ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| native | 96 | 92.8099 | 6121.0 | 40.7 | 39.4 | 46.6 | 116.1 |
| ext-grpc 1.58 optimized | 96 | 130.7353 | 6228.9 | 30.0 | 28.6 | 36.0 | 49.2 |

判断: wall latency / RPSではext-grpcが速いが、CPU/request差は再現していない。

### transaction_select2_update1_insert1 c16, 192 requests

Command:

```bash
COMPOSE_FILE=compose.yaml:var/fpm-sa-json-ext158.override.yaml \
LARAVEL_SPANNER_EMULATOR_HOST= \
LARAVEL_SPANNER_PROJECT_ID=vast-falcon-165704 \
LARAVEL_SPANNER_INSTANCE_ID=bench \
LARAVEL_SPANNER_DATABASE_ID=laravel-bench-db \
BENCH_ACTIONS=transaction_select2_update1_insert1 \
BENCH_RUN_ID=sa-json-ext158-mixed-c16-20260520 \
./bench/fpm-laravel-spanner-load-compare.sh 192 16
```

| variant | requests | rps | cpu_us/req | avg_ms | p50_ms | p90_ms | max_ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| native | 192 | 15.1890 | 21386.6 | 1042.5 | 1217.7 | 1500.8 | 1759.0 |
| ext-grpc 1.58 optimized | 192 | 28.1263 | 15894.7 | 516.7 | 484.3 | 945.6 | 1688.9 |

判断: このrunではCPU/request比が約1.35x、wall latencyでもnativeが大きく遅い。

### transaction_select2_update1_insert1 c16, 192 requests repeat

Command:

```bash
COMPOSE_FILE=compose.yaml:var/fpm-sa-json-ext158.override.yaml \
LARAVEL_SPANNER_EMULATOR_HOST= \
LARAVEL_SPANNER_PROJECT_ID=vast-falcon-165704 \
LARAVEL_SPANNER_INSTANCE_ID=bench \
LARAVEL_SPANNER_DATABASE_ID=laravel-bench-db \
BENCH_ACTIONS=transaction_select2_update1_insert1 \
BENCH_RUN_ID=sa-json-ext158-mixed-c16-repeat2-20260520 \
./bench/fpm-laravel-spanner-load-compare.sh 192 16
```

| variant | requests | rps | cpu_us/req | avg_ms | p50_ms | p90_ms | max_ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| native | 192 | 27.5023 | 13688.0 | 499.4 | 493.2 | 765.0 | 1442.7 |
| ext-grpc 1.58 optimized | 192 | 27.6593 | 15346.8 | 544.7 | 510.3 | 831.5 | 1430.8 |

判断: 反復ではCPU/request差もwall latency差も消えた。

### transaction_select2_update1_insert1 c16, 512 requests

Command:

```bash
COMPOSE_FILE=compose.yaml:var/fpm-sa-json-ext158.override.yaml \
LARAVEL_SPANNER_EMULATOR_HOST= \
LARAVEL_SPANNER_PROJECT_ID=vast-falcon-165704 \
LARAVEL_SPANNER_INSTANCE_ID=bench \
LARAVEL_SPANNER_DATABASE_ID=laravel-bench-db \
BENCH_ACTIONS=transaction_select2_update1_insert1 \
BENCH_RUN_ID=sa-json-ext158-mixed-c16-long-20260520 \
./bench/fpm-laravel-spanner-load-compare.sh 512 16
```

| variant | requests | rps | cpu_us/req | avg_ms | p50_ms | p90_ms | max_ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| native | 512 | 29.1734 | 14637.8 | 541.3 | 539.5 | 623.8 | 714.0 |
| ext-grpc 1.58 optimized | 512 | 28.2000 | 14524.6 | 559.0 | 563.7 | 629.4 | 718.3 |

判断: 長めの固定件数ではCPU/requestはほぼ同等。SA JSONだけでLaravel/FPMのCPU 1.5x差が安定再現するとは言えない。

## 現時点の判断

- SA JSON条件でも、Laravel/FPM runnerではCPU/request 1.5x差は安定再現していない。
- `select_1row_10col` ではext-grpcのwall latency/RPS優位は出たが、CPU/request差はない。
- mixed transactionでは単発で1.35x差が出たが、反復と512 requestsでは消えた。
- `ExecuteStreamingSql SELECT 1` minimal reproで見えているSA JSON差は、Laravel/FPM全体のCPU/request差へ単純には変換されない。FPM/Laravel/Spanner transaction待ち/セッション状態の揺れが大きい。
- 次に追うなら、CPU/requestではなく、SA JSON + `select_1row_10col` のwall latency/RPS差を起点に、request送信後からfirst responseまでのwire timingをLaravel/FPM経路でも見る。

## 完了条件

- SA JSON条件でLaravel/FPM real Spanner CPU/request差が再現するか判断できる。
- 再現する場合は次に見るべき計測軸を分離できる。
