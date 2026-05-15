---
Status: Open
Owner: Codex
Created: 2026-05-16
Branch: main
---

# ext-grpc 1.58.0固定でCPU差が見える負荷条件を探す

## 目的

実アプリで観測された `php-grpc-lite` が `ext-grpc` 比でCPU約1.5倍になる条件について、比較対象の公式 `ext-grpc` を実アプリと同じ `1.58.0` に揃えた上で、real Spanner + Laravel/FPM fixtureで再現条件を探す。

## 背景

これまでの比較では公式 `ext-grpc` のバージョンが実アプリ条件と一致していなかった。実アプリ側の比較対象は `ext-grpc 1.58.0`。追加確認により、実アプリ側では `ext-grpc 1.58.0` が LTO / O3 / `-fno-semantic-interposition` 付き、`php-grpc-lite` は通常ビルドで GCC 15 のみだった。まずはバージョンだけを合わせ、1.5倍差が見えるかを確認する。

## スコープ

- 公式 `ext-grpc 1.58.0` の `grpc.so` を取得する。
- FPM fixtureで公式 `ext-grpc 1.58.0` のsoを読み込めるようにする。
- real Spanner single select / single DML / mixed transactionでworker/client並列を振る。
- CPU/request差が大きくなる条件を探す。

## 非スコープ

- 実アプリの非対称ビルド条件の完全再現。
  - `ext-grpc 1.58.0`: GCC 15 + LTO + O3 + `-fno-semantic-interposition`
  - `php-grpc-lite`: GCC 15のみの通常ビルド
- 公式ext-grpc 1.58.0のソース改変。
- 上位レイヤCPU breakdown。

## 計画

1. `ext-grpc 1.58.0` のsoをビルドする。
2. `EXT_GRPC_SO` または既存のso差し替え導線でFPMへ投入する。
3. 低並列から高並列まで条件を振ってCPU/requestを比較する。
4. 1.5倍差が見える/見えないを記録する。

## 進捗

- `tools/dev/build-official-ext-grpc-so.sh 1.58.0` 相当の手順で公式 `ext-grpc 1.58.0` をビルドし、`var/official-ext-grpc-so/1.58.0/grpc.so` を作成した。
- FPM fixtureでは `NATIVE_GRPC_SO=/workspace/var/official-ext-grpc-so/1.58.0/grpc.so` を指定し、`php -n -d extension=...` で `phpversion("grpc") === 1.58.0` を確認した。
- `grpc-lite` は現行 `main` の `/workspace/ext/grpc/modules/grpc.so`、比較対象は公式 `ext-grpc 1.58.0` のso。今回はバージョン合わせのみで、実アプリのビルド条件差は未反映。
- 現在のdevコンテナは `gcc (Debian 14.2.0-19) 14.2.0` / `PHP 8.4.20`。実アプリで使った `GCC 15` 条件にもまだ揃っていない。

## 検証

### 低〜高並列探索

real Spanner / Laravel FPM / 16 workers / 4 CPU limit / worker warmupあり。

| concurrency | action | grpc-lite cpu_us/req | ext-grpc 1.58 cpu_us/req | ratio |
|---:|---|---:|---:|---:|
| 1 | select_1row_10col | 10005.6 | 8387.8 | 1.19x |
| 2 | select_1row_10col | 8921.0 | 9422.5 | 0.95x |
| 4 | select_1row_10col | 10803.4 | 7834.1 | 1.38x |
| 8 | select_1row_10col | 9226.2 | 8635.1 | 1.07x |
| 16 | select_1row_10col | 10263.6 | 9584.1 | 1.07x |
| 32 | select_1row_10col | 11727.9 | 9444.1 | 1.24x |
| 4 | dml_insert_10col | 8042.0 | 7803.6 | 1.03x |
| 16 | dml_insert_10col | 8951.6 | 8080.6 | 1.11x |
| 32 | dml_insert_10col | 8800.8 | 7855.8 | 1.12x |
| 4 | transaction_select2_update1_insert1 | 13503.6 | 10411.5 | 1.30x |

### c4 select反復

`select_1row_10col` / 128 requests / concurrency 4 を3回反復。

| iter | grpc-lite cpu_us/req | ext-grpc 1.58 cpu_us/req | ratio | 備考 |
|---:|---:|---:|---:|---|
| 1 | 28386.1 | 8245.3 | 3.44x | grpc-lite側だけp90/maxが大きい外れ値 |
| 2 | 8733.2 | 8991.4 | 0.97x | 差なし |
| 3 | 11265.5 | 11355.6 | 0.99x | 差なし |

### 判断

- 単発では `c4 select` で1.38x、反復1回目で3.44xが出たが、反復2〜3回目では差が消えており、現時点では「1.5x CPU差が安定再現する負荷条件」とは言えない。
- `dml_insert_10col` はc4〜c32で1.03〜1.12x程度で、1.5x差の主因には見えていない。
- mixed transactionは1.30xで、select系より差が大きい可能性はあるが、まだ1.5xには届いていない。
- 次に再現性を上げるなら、バージョンだけでなく実アプリ条件の非対称ビルド差、CPU制限、worker/client比、長時間sustainでCPU集計窓を広げる必要がある。

## 完了条件

- 公式 `ext-grpc 1.58.0` 固定の比較結果がある。
- 1.5倍差の再現有無と次に見るべき条件が説明できる。

## 次に見る候補

1. 実アプリの非対称ビルド条件を合わせる。
   - `ext-grpc 1.58.0`: GCC 15 + O3 + LTO + `-fno-semantic-interposition`
   - `php-grpc-lite`: GCC 15 通常ビルド
2. `transaction_select2_update1_insert1` を長めに反復し、CPU/requestの揺れを下げる。
3. FPM CPU limit / worker数 / client concurrencyを実アプリ条件に寄せる。
4. `perf` / `callgrind` は、差が安定再現する条件が見えた後に限定して実施する。
