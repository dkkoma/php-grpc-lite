# Benchmark Guide

このディレクトリはベンチ結果の記録、`bench/` は実行対象、`bench/run.sh` は再現可能な実行入口を置く。

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

## 実用性能として押さえる軸

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

### 追加整備する性能軸

gRPC 仕様や実運用から見て性能比較として追加する価値があるものだけをここに残す。deadline、cancellation、trailers-only、HTTP status 合成などの制御/互換性項目は `docs/compatibility-control-checklist.md` に分離する。

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

`BENCH_BASELINE` を指定すると、php-grpc-lite 側の抽出済み JSON を baseline と比較する。

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

## ベンチ文書を書く基準

- 対向サーバ、実行コマンド、代表値、揺れ幅を残す。
- ext-grpc は目標値ではなく比較線として扱う。
- Spanner emulator は実機互換検証には使うが、性能比較では Go test-server を優先する。
- cold と warm を混ぜない。request 内で Channel を再利用できる workload は warm、request ごとに 1 RPC の workload は cold を参照する。

## Phase 1 の残り

- CI で回すスモークベンチのしきい値を決める。
- regression baseline に載せた smoke 対象を CI/manual でどう回すか決める。
- メモリ指標は PHPBench aggregate の `mem_peak` を記録しているが、回帰判定にはまだ使っていない。
