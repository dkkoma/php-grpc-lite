# Franken-go channel pool benchmark 2026-05-13

## Context

`github.com/dkkoma/frankenphp-grpc-go-client` に channel pool が入った後の `franken-go` backend を、php-grpc-lite `main` の native backend と公式 ext-grpc に並べて再計測した。

- php-grpc-lite commit: `d878817` base + local `otelop-summary.php` variant grouping fix
- frankenphp-grpc-go-client: `github.com/dkkoma/frankenphp-grpc-go-client@main` resolved by xcaddy as `v0.0.0-20260513131237-cace7741d35d`
- server: compose `test-server`
- summary source: OTEL spans in `otelop`, summarized by `tools/benchmark/otelop-summary.php`
- note: `variant=native` is php-grpc-lite nghttp2 transport, `variant=franken-go` is FrankenPHP grpc-go backend, `variant=ext-grpc` is official ext-grpc.

## Verification commands

```bash
docker compose build --no-cache dev-franken-grpc-go
docker compose run --rm dev-franken-grpc-go tools/test/check-franken-go-backend.sh
```

```bash
BENCH_TAG=franken-pool-dml-20260513-132200 \
BENCH_OTEL_RUN_ID=franken-pool-dml-20260513-132200 \
INCLUDE_FRANKEN=1 \
DURATION=30 WARMUP_CALLS=10 MAX_CALLS=1000 \
BENCH_OTEL_SUMMARY_LIMIT=20000 \
./bench/compare-spanner-dml-unary-shape.sh
```

```bash
BENCH_TAG=franken-pool-small-select-20260513-132300 \
BENCH_OTEL_RUN_ID=franken-pool-small-select-20260513-132300 \
INCLUDE_FRANKEN=1 \
WARMUP_STREAMS=10 \
BENCH_OTEL_SUMMARY_LIMIT=20000 \
./bench/compare-small-select-streaming.sh
```

Additional throughput spot checks used the same OTEL runner boundary:

```bash
BENCH_OTEL_RUN_ID=franken-pool-throughput-short-20260513-132500 \
./bench/run.sh throughput-unary --duration=0.2 --payload-bytes=100
```

```bash
BENCH_OTEL_RUN_ID=franken-pool-stream-throughput-20260513-132600 \
./bench/run.sh throughput-streaming --duration=3 --message-count=1000 --payload-bytes=100 --warmup-streams=3
```

## Spanner DML unary shape

| case | native p50/p99 us | franken-go p50/p99 us | ext-grpc p50/p99 us | note |
|---|---:|---:|---:|---|
| begin_txn | 27.0 / 190.7 | 57.6 / 330.9 | 54.7 / 127.6 | franken-go p50はext-grpc近似、p99は劣後 |
| dml_insert_10col | 25.4 / 96.6 | 60.9 / 187.5 | 53.3 / 231.2 | franken-go p99はext-grpcより良いがnativeより遅い |
| dml_update_10col | 27.3 / 104.7 | 72.5 / 216.7 | 45.1 / 100.0 | franken-goはp50/p99とも劣後 |
| dml_delete_10col | 23.0 / 66.0 | 72.1 / 240.2 | 58.5 / 115.8 | franken-goはp50/p99とも劣後 |
| commit_txn | 25.7 / 90.9 | 70.6 / 323.5 | 53.8 / 105.9 | franken-goはp50/p99とも劣後 |

Previous OTEL Spanner shape result from `docs/issues/closed/2026-05-10-otel-spanner-shape-remeasurement.md` had native p50 around 35-53us and ext-grpc p50 around 62-101us depending on case. 今回も主要な傾向は変わらず、native は small unary p50 で優位。franken-go channel pool後も、このsynthetic Spanner DML shapeでは native を置き換えるほどの優位は出ていない。

## Small SELECT streaming shape

| payload | native p50/p99 us | franken-go p50/p99 us | ext-grpc p50/p99 us | note |
|---|---:|---:|---:|---|
| 100B | 46.3 / 390.1 | 98.9 / 937.4 | 90.7 / 1041.4 | franken-go p50はext-grpcより少し遅く、p99は少し良い |
| 1KiB | 45.2 / 621.4 | 94.3 / 716.5 | 93.0 / 848.8 | franken-goはext-grpc近似、nativeより遅い |
| 4KiB | 47.0 / 1006.4 | 98.2 / 1201.3 | 90.0 / 1200.3 | franken-goはext-grpc近似、nativeより遅い |
| 10KiB | 49.5 / 1233.1 | 103.7 / 1005.6 | 98.1 / 1098.5 | franken-go p99はnative/ext-grpcより良いがp50は遅い |

Previous OTEL small select result had native p50 around 48-53us and ext-grpc p50 around 94-101us. 今回も native p50 の優位は維持。franken-go は channel pool後に ext-grpc 近辺の固定費に入っているが、native の small streaming p50 には届いていない。

## Throughput spot checks

| suite | shape | native p50/p99 us | franken-go p50/p99 us | ext-grpc p50/p99 us | note |
|---|---|---:|---:|---:|---|
| throughput-unary | payload=100, duration=0.2s | 26.8 / 87.4 | 61.0 / 385.1 | 58.2 / 158.6 | franken-go p50はext-grpc近似、p99は劣後 |
| throughput-streaming | 1000x100B, duration=3s | 963.5 / 2609.3 | 1367.1 / 3130.3 | 2548.0 / 3505.2 | franken-goはext-grpcより速いがnativeより遅い |

`throughput-unary --duration=3` は現在の `BenchTelemetry` が1 RPC = 1 spanを全件メモリに保持するため、nativeでPHP memory limitに到達した。上のunary throughputは短時間spot checkとして扱う。

## Conclusion

- channel pool後の franken-go backend は、以前疑っていた「RPCごとにchannel相当の固定費を払う」状態ではなくなっている可能性が高い。
- ただし、Spanner主用途で重要な small unary / small server streaming では、現時点の main native backend が p50 で一貫して最速。
- franken-go は ext-grpc に近い、またはstreamingではext-grpcより良いケースがあるが、php-grpc-lite native を通常defaultから置き換える根拠にはならない。
- franken-go backend は FrankenPHP環境で grpc-go の成熟したchannel/pool semanticsを使いたい場合の optional backend として残すのが妥当。
- benchmark infra上の課題として、高throughput unaryをOTEL span全件保持で長時間回すとメモリ上限に当たる。長時間throughputを継続的に見るなら、span exportのchunk flushまたはsampling/summary span化が必要。
