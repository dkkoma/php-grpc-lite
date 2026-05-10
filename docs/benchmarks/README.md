# Benchmark Guide

このディレクトリはベンチ結果の記録、`bench/` は実行対象、`bench/run.sh` は再現可能な通常比較入口、`bench/baseline.sh` は php-grpc-lite 自身の regression baseline 運用入口を置く。

## 通常の比較線

通常の継続比較は **php-grpc-lite vs 公式 ext-grpc** に固定する。Rust 製 drop-in 代替候補 `grpc-php-rs` は one-shot / 明示依頼時だけ `bench/compare-rs.sh` で使う。

```bash
./bench/run.sh compare
```

互換のため、従来の入口も同じ比較線を実行する。

```bash
./bench/compare.sh
```

どちらも `var/bench-results/` にログを保存し、PHPBench aggregate については同名の `.json` / `.tsv` も生成する。保存名は `BENCH_TAG` と `BENCH_OUTPUT_DIR` で固定できる。PHPBench 実行、aggregate parse、任意の baseline compare は同じコンテナ内の `bench/phpbench-with-artifacts.sh` で完結させ、ホストに書いたログを直後に別コンテナから読み直す構成にはしない。

```bash
BENCH_TAG=20260427-local ./bench/run.sh compare
BENCH_OUTPUT_DIR=/tmp/php-grpc-lite-bench ./bench/run.sh cold
```

## スイート

| command | 対象 | 用途 |
|---|---|---|
| `./bench/run.sh lite` | php-grpc-lite full PHPBench | 実装側だけの全体確認 |
| `./bench/run.sh ext` | 公式 ext-grpc full PHPBench | 比較対象だけの確認 |
| `./bench/run.sh compare` | php-grpc-lite + 公式 ext-grpc | 通常比較 |
| `./bench/run.sh cold` | `ColdUnaryBench` 両環境 | PHP-FPM request boundary 近似 |
| `./bench/run.sh warm` | `UnaryLatencyBench` 両環境 | request 内 Channel reuse が効く軽量 unary |
| `./bench/run.sh stream` | `ServerStreamingBench` 両環境 | server streaming の per-message / per-byte / pacing |
| `./bench/run.sh stream-smoke` | `ServerStreamingCount1000Bench` 両環境 | server streaming count=1000 の回帰 smoke |
| `./bench/run.sh stream-slow` | `ServerStreamingSlowConsumerBench` 両環境 | slow consumer 時の elapsed / mem_peak |
| `./bench/run.sh metadata` | `MetadataVolumeBench` 両環境 | request / response metadata volume の固定費 |
| `./bench/run.sh tls` | `TlsUnaryBench` 両環境 | TLS / mTLS の cold/warm unary |
| `./bench/run.sh hot-path` | `tools/bench-hot-path.php` | ネットワークなしの CPU 分解 |

## Phase 2 探索ベンチ

Phase 2 の探索ベンチは、通常比較入口の `bench/run.sh` と regression baseline 入口の `bench/baseline.sh` から分離し、`bench/phase2/run.sh` で実行する。

```bash
./bench/phase2/run.sh contract-smoke
./bench/phase2/run.sh cpu-memory-smoke
./bench/phase2/run.sh throughput-unary
./bench/phase2/run.sh rtt-unary
./bench/phase2/run.sh rtt-unary-diagnostic
./bench/phase2/run.sh throughput-streaming
./bench/phase2/run.sh large-streaming
./bench/phase2/run.sh payload-unary
./bench/phase2/run.sh payload-unary-diagnostic
./bench/phase2/run.sh payload-unary-diagnostic-cached
./bench/phase2/run.sh payload-unary-return-transfer-fast-path
./bench/phase2/run.sh payload-breakdown
./bench/phase2/run.sh payload-streaming
./bench/phase2/run.sh metadata-header
./bench/phase2/run.sh metadata-header-diagnostic
```

suite 固有の引数は suite 名の後ろに渡せる。

```bash
./bench/phase2/run.sh throughput-unary --duration=5 --payload-bytes=100
./bench/phase2/run.sh rtt-unary --calls=30 --payload-bytes=100
./bench/phase2/run.sh large-streaming --message-counts=10000,100000
```

同じ Phase 2 suite を php-grpc-lite と公式 ext-grpc の両方で実行する場合は比較入口を使う。

```bash
./bench/phase2/compare.sh throughput-unary --duration=3
./bench/phase2/compare.sh rtt-unary --calls=20
```

複数 suite を目的別にまとめて実行する場合は preset 入口を使う。

```bash
./bench/phase2/preset.sh smoke
./bench/phase2/preset.sh compare
./bench/phase2/preset.sh decision
```

| preset | 用途 | 性質 |
|---|---|---|
| `smoke` | runner contract と代表比較の高速確認 | 短時間。壊れていないかを見る |
| `compare` | Phase 2 軸と Spanner shape を一通り ext-grpc と短時間比較 | preliminary な傾向確認 |
| `decision` | 最適化判断に使う長めの比較 | p99 / large streaming / metadata / Spanner shape の外れ値影響を下げる |

単独実行で比較対象を切り替える場合は `BENCH_IMPLEMENTATION=ext-grpc` を指定する。ext-grpc 側は `dev-ext-grpc` と `vendor/autoload.php` を使う。

Phase 2 runner は PHPBench aggregate JSON と別 contract の JSON を出す。schema は `docs/benchmarks/schemas/phase2-result-v1.md` を参照する。探索結果は `bench/baselines/regression.json` に混ぜない。

Phase 2 の primary metric は wall time、throughput、tail latency、memory とする。JSON に入る `diagnostic_cpu_*` は参考値であり、合否や優先度判断の主指標にはしない。

Phase 2 runner は任意で `otelop` へOTLP/HTTP exportできる。通常の結果JSONは従来通り保存しつつ、PHP runner側の共通RPC境界を1 RPC = 1 spanとしてUIで見る用途に使う。

```bash
docker compose up -d otelop

BENCH_OTEL_EXPORTER=otlp-http \
BENCH_OTEL_EXPORTER_OTLP_ENDPOINT=http://otelop:4318/v1/traces \
./bench/phase2/run.sh payload-unary-diagnostic --duration=0.2 --max-calls=5 --payload-sizes=100
```

UIは `http://localhost:4319`。OTLP/HTTP endpointはcompose内から `http://otelop:4318/v1/traces`、ホストから直接送る場合は `http://localhost:4318/v1/traces` を使う。

`throughput-unary` は単一 PHP process / concurrency=1 で `BenchUnary` を duration 中に回し続け、calls/sec と p50/p95/p99 を保存する。

`rtt-unary` は `toxiproxy` service を起動し、direct と downstream latency 1 / 3 / 5 ms の unary を測る。`rtt-unary-diagnostic` は同じ条件で php-grpc-lite の curl timing を保存する。これは同一ホスト上での探索用近似であり、実ネットワーク RTT の完全再現ではない。

`throughput-streaming` / `large-streaming` / `payload-streaming` は `BenchServerStream` を使い、message/sec、stream latency、memory を保存する。`compare-small-select-streaming.sh` は 1 stream = 1 message の小さな server streaming response を測り、Spanner `ExecuteStreamingSql` が 1 `PartialResultSet` で返る小さな SELECT を近似する。`payload-unary` は unary payload size 別の calls/sec と tail latency、`compare-spanner-dml-unary-shape.sh` は Spanner DML flow の BeginTransaction / ExecuteSql DML / Commit に近い request-response size を測る。`payload-unary-diagnostic` は php-grpc-lite の実 unary RPC 内で curl / body append / frame parse / deserialize の opt-in diagnostic、`payload-unary-diagnostic-cached` は Go test-server 側の payload allocation を事前生成 payload で外した診断、`payload-unary-return-transfer-fast-path` は unary response body の `CURLOPT_WRITEFUNCTION` を外す参考計測、`request-unary-diagnostic` は large request / small response の upload 側 cost を測る。`payload-unary-diagnostic*` と `request-unary-diagnostic` は `x-bench-server-stats: 1` による grpc-go `stats.Handler` の server-side event timing も保存する。server stats は ext-grpc 実行時にも取得し、php-grpc-lite 固有の curl / userland diagnostics は php-grpc-lite 実行時だけ取得する。大きい payload sweep では runner 側の全 sample 保持を抑えるため `--max-calls=1000` のように call 数で止める。少数 call の libcurl debug trace が必要な場合だけ `--curl-trace-output=var/bench-results/trace.log --curl-trace-calls=2` を追加する。trace 有効時の latency は参考値で、通常比較や regression baseline には使わない。`payload-breakdown` はネットワークを外した frame length / payload slice / protobuf decode の diagnostic hot path、`metadata-header` は request / initial / trailing metadata 数別の unary latency、`metadata-header-diagnostic` は同条件の request header build / response header callback 内訳を保存する。

## 実用性能として押さえる軸

Phase 1 の手元運用・継続比較基盤は完了扱いとする。以後この文書で追加する作業は、原則として CI での実行方針に限定する。

ベンチマークの目的は HTTP/2 DATA frame や libcurl write callback の chunk 境界そのものを固定して比べることではなく、gRPC ライブラリの実利用で性能差になる条件を押さえること。chunk 境界は実装内部の診断対象であり、通常比較の主指標にはしない。

| 軸 | 現在の対象 | 見たい差 |
|---|---|---|
| request 境界 / connection lifetime | `cold`, `warm` | PHP-FPM で request ごとに 1 RPC の場合と、request 内で Channel reuse できる場合の差 |
| 軽量 unary 固定費 | `warm` | metadata / framing / curl dispatch / protobuf decode の最小固定費 |
| unary payload size | `compare` の `UnaryPayloadBench` | payload が大きい場合の per-byte decode / copy cost |
| server streaming message count | `stream`, `stream-smoke` | 1 stream 内で message 数が増えた場合の per-message overhead |
| server streaming payload size | `stream` | streaming で payload が大きい場合の per-byte cost |
| server-paced streaming | `stream` の `benchPacedDelivery` | サーバが message 間隔を空ける実運用寄りの cadence で Generator / `curl_multi_*` がどう振る舞うか |
| server delay unary | `compare` の `UnaryDelayBench` | 実装固定費よりサーバ処理時間が支配的な場合に差がどれだけ残るか |
| CPU hot path | `hot-path` | ネットワークを外した framing / header parse / protobuf merge の上限コスト |

### 将来の性能軸

gRPC 仕様や実運用から見て将来の性能比較として追加する価値があるものだけをここに残す。deadline、cancellation、trailers-only、HTTP status 合成などの制御/互換性項目は `docs/compatibility-control-checklist.md` に分離する。

| 軸 | 計測方法 | suite 化する時の注意 |
|---|---|---|
| slow consumer streaming | `stream-slow`。server streaming を受け取り、各 response 後に client 側で固定 sleep する | elapsed time だけでなく peak memory を保存する。chunk 境界の診断ログは異常値の原因切り分け用 |
| metadata volume | `metadata`。request metadata / initial metadata / trailing metadata の key 数と value サイズを増やす unary | ASCII metadata と `*-bin` metadata を分ける。auth/tracing に近い小さい metadata 多数を優先 |
| TLS / mTLS | `tls`。h2 TLS / mTLS listener で cold/warm unary を測る | handshake コストと warm call コストを混ぜない。mTLS は証明書読み込み方式も記録する |
| concurrent streams | 同一 Channel から複数 unary / streaming を in-flight にする専用 bench | 現行 API で自然に表現できる範囲を先に確認する。ext-grpc と比較する場合は同一 concurrency に固定 |
| compression enabled | gzip 等を実装した後に compressed/uncompressed を比較 | 未対応の間は性能 bench ではなく互換性 smoke で明示エラーを確認する |

HTTP/2 DATA frame や libcurl write callback の chunk 境界は、通常比較の主指標にはしない。slow consumer や streaming count で不自然な結果が出た場合に、chunk サイズ、chunk 内 frame 数、frame 分割回数を記録する補助 instrumentation として使う。

## 生成物

`./bench/run.sh cold` を `BENCH_TAG=local` で実行すると、以下のようなファイルができる。

```text
var/bench-results/cold-local-php-grpc-lite.log
var/bench-results/cold-local-php-grpc-lite.json
var/bench-results/cold-local-php-grpc-lite.tsv
var/bench-results/cold-local-ext-grpc.log
var/bench-results/cold-local-ext-grpc.json
var/bench-results/cold-local-ext-grpc.tsv
```

手動で既存ログを変換する場合は以下を使う。

```bash
docker compose run --rm dev php tools/parse-phpbench-aggregate.php \
  --format=json \
  --output=var/bench-results/result.json \
  var/bench-results/result.log
```

## Regression Baseline

回帰判定用 baseline は `bench/baselines/regression.json` に置く。これは historical baseline ではなく、意図した改善や環境変更後に明示更新してコミットする基準値。

通常運用は `bench/baseline.sh` を使う。対象は `cold`、`warm`、`stream-smoke` の php-grpc-lite 側だけで、公式 ext-grpc は実行しない。

```bash
./bench/baseline.sh check
```

意図した性能変化や実行環境変更を受け入れる場合だけ、現在値から baseline を更新する。

```bash
./bench/baseline.sh update
git diff bench/baselines/regression.json
```

`check` は `mode_ns` と `mem_peak_bytes` を判定する。デフォルトしきい値は `bench/baselines/regression.json` の `default_thresholds` に置く。

| threshold | 既定値 | 意味 |
|---|---:|---|
| `mode_warn_percent` | `15.0` | latency regression warning |
| `mode_fail_percent` | `25.0` | latency regression failure |
| `mem_peak_warn_percent` | `5.0` | peak memory regression warning |
| `mem_peak_fail_percent` | `15.0` | peak memory regression failure |
| `max_rstdev_percent` | `15.0` | noisy run warning |

`BENCH_TAG`、`BENCH_OUTPUT_DIR`、`BENCH_BASELINE` は通常比較と同じように使える。

```bash
BENCH_TAG=local ./bench/baseline.sh check
BENCH_BASELINE=bench/baselines/regression.json ./bench/baseline.sh update
```

`BENCH_BASELINE` を指定して `bench/run.sh` の単独 suite を実行すると、従来通り php-grpc-lite 側の抽出済み JSON を baseline と比較できる。この場合は ext-grpc も比較線として実行される。

```bash
BENCH_BASELINE=bench/baselines/regression.json ./bench/run.sh cold
BENCH_BASELINE=bench/baselines/regression.json ./bench/run.sh warm
BENCH_BASELINE=bench/baselines/regression.json ./bench/run.sh stream-smoke
```

手動比較もできる。

```bash
docker compose run --rm dev php tools/compare-benchmark-baseline.php \
  --baseline=bench/baselines/regression.json \
  --current=var/bench-results/cold-local-php-grpc-lite.json \
  --suite=cold \
  --implementation=php-grpc-lite
```

baseline は php-grpc-lite 自身の回帰検知用で、公式 ext-grpc との差を fail 判定するためには使わない。ext-grpc は継続比較線として観測する。

baseline 更新時のルール:

- benchmark 実装、Docker image、ホスト環境の大きな変更を受け入れる時だけ更新する。
- 一時的な速い/遅い run を拾わない。`rstdev_percent` が高い run は採用しない。
- `bench/baselines/regression.json` は更新理由が分かるコミットで単独または小さくまとめる。
- 性能の歴史記録は baseline ではなく `docs/benchmarks/*.md` に残す。

## ベンチ文書を書く基準

- 対向サーバ、実行コマンド、代表値、揺れ幅を残す。
- ext-grpc は目標値ではなく比較線として扱う。
- Spanner emulator は実機互換検証には使うが、性能比較では Go test-server を優先する。
- cold と warm を混ぜない。request 内で Channel を再利用できる workload は warm、request ごとに 1 RPC の workload は cold を参照する。

## 記録済みの比較

- [Phase 2 preliminary comparison 2026-04-28](./phase2-preliminary-comparison-2026-04-28.md)
- [Phase 2 decision comparison 2026-04-29](./phase2-decision-comparison-2026-04-29.md)

## Phase 1 の完了

Phase 1 は 2026-04-28 時点で完了扱いとする。ローカルで再現可能な通常比較、公式 ext-grpc 比較、php-grpc-lite 自身の regression baseline check/update までを Phase 1 のスコープとする。

CI 上で `./bench/baseline.sh check` をどのタイミングで実行するかは、Phase 1 の残作業ではなく通常の保守運用タスクとして扱う。

Phase 2 のスコープ決定に必要な追加計測は `docs/benchmarks/measurement-plan-phase2.md` にまとめる。
