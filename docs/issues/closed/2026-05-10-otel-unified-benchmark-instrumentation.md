# OpenTelemetryをライブラリ内部観測とベンチ計測の共通基盤にする

- Status: Closed
- Created: 2026-05-10
- Branch: feature/opentelemetry-bench
- Owner: Codex

## Background

現在のOpenTelemetry対応は、C拡張の内部telemetry recordをPHP callbackへ渡し、`OpenTelemetryHandler` やbench専用 `BenchTelemetry` で扱う構成になっている。

ただし、現行のbench専用 `BenchTelemetry` は1 RPCごとに同期OTLP/HTTP POSTする実装であり、ベンチのwall timeにexport overheadが入りやすい。また、php-grpc-lite固有の内部recordだけをspan化するため、ext-grpcやfranken-goと同じ計測モデルになっていない。

目的は、ライブラリ利用時の内部観測とベンチ計測をOpenTelemetryへ統一すること。

## Goals

1. ライブラリ利用側・アプリケーション側が有効化したときに、php-grpc-liteの内部状態をOpenTelemetry形式で観測できるようにする。
2. ライブラリ内部の独自診断計測をOpenTelemetry record/span/event/attributeへ寄せる。
3. ベンチマークもphp-grpc-lite、ext-grpc、franken-goを同じOpenTelemetry span境界で測る。
4. JSON/TSVを一次ソースにせず、必要な集計はOTEL backendまたはOTEL query結果から得る。
5. otelopでSpanner shapeなど主要ベンチのspanを見られ、p50/p99などを集計できるようにする。

## Non-Goals

- C拡張がOTLP exporter、sampling、batch、retry、flushを直接実装すること。
- ext-grpc内部のC-core詳細timingをphp-grpc-liteと同じ粒度で取得すること。
- ベンチ結果の互換JSON/TSVを永続的な一次成果物として維持すること。

## Desired Model

Production / application path:

```text
application root span
└── application RPC span or existing active span
    └── php-grpc-lite internal attributes/events
        - grpc_lite.duration_us
        - grpc_lite.setup_us
        - grpc_lite.submit_us
        - grpc_lite.initial_send_us
        - grpc_lite.recv_loop_us
        - grpc_lite.bytes_sent / bytes_received
        - grpc_lite.http2.*
        - grpc_lite.connection.*
```

Benchmark path:

```text
measurement span: spanner-dml-unary-shape / dml_insert_10col / php-grpc-lite
└── rpc span #1
    ├── wall time span duration
    ├── rpc.system / service / method
    ├── grpc.status_code
    ├── benchmark.* attributes
    └── grpc_lite.* internal attributes/events only when implementation=php-grpc-lite
└── rpc span #2
└── ...
```

ext-grpc and franken-go also get the same outer measurement/rpc spans, but without php-grpc-lite internal attributes.

## Plan

1. Replace bench-specific synchronous OTLP exporter with a bench span recorder that batches OTLP export after measurement.
2. Add shared benchmark instrumentation helpers for per-RPC spans.
3. Make Spanner unary and server-streaming benchmark helpers create common outer RPC spans for all implementations.
4. Attach php-grpc-lite C telemetry records to the active RPC span.
5. Add otelop/OTEL query CLI to aggregate p50/p99 from spans.
6. Decide how much of legacy JSON/TSV output remains as derived/compat output.

## Progress

- 2026-05-10: Current `BenchTelemetry` can export php-grpc-lite internal records to otelop, but this is not the desired final model because it is synchronous and not shared by ext-grpc.
- 2026-05-10: Spanner DML unary and small select streaming were run with the current exporter; results confirmed otelop visibility but also showed that TSV wall time includes exporter overhead.
- 2026-05-10: `BenchTelemetry` was replaced with a batched span recorder. It records common outer RPC spans for all implementations and attaches php-grpc-lite C telemetry records to the active span only for php-grpc-lite.
- 2026-05-10: `unary-shape.php` and `streaming-diagnostic.php` now create per-RPC spans around the actual RPC call boundary.
- 2026-05-10: `otelop-summary.php` was added to query otelop GraphQL and compute p50/p99 grouped by run id, suite, measurement, shape, and implementation.
- 2026-05-10: `otelop` trace cap was raised to 20000 via compose so multi-case streaming runs can be summarized after completion.
- 2026-05-10: `UnaryBenchHelper` and `StreamingBenchHelper` now provide telemetry-wrapped RPC helpers. Existing payload, request, RTT, metadata, throughput, and streaming Phase 2 RPC runners use those wrappers for measurement calls.

## Verification

- `docker compose run --rm dev sh -lc 'for f in tools/phase2/BenchTelemetry.php tools/phase2/unary-shape.php tools/phase2/streaming-diagnostic.php tools/phase2/otelop-summary.php; do php -l "$f" || exit 1; done'`
- `docker compose up -d --force-recreate otelop`
- `BENCH_OTEL_RUN_ID=otel-unified-smoke-20260510-111017 BENCH_OTEL_EXPORTER=otlp-http BENCH_OTEL_EXPORTER_OTLP_ENDPOINT=http://otelop:4318/v1/traces DURATION=0.05 WARMUP_CALLS=1 MAX_CALLS=5 ./bench/phase2/compare-spanner-dml-unary-shape.sh`
- `docker compose run --rm -e BENCH_OTEL_RUN_ID=otel-unified-smoke-20260510-111017 dev php tools/phase2/otelop-summary.php --run-id=otel-unified-smoke-20260510-111017 --suite=spanner-dml-unary-shape --limit=100000`
- `BENCH_OTEL_RUN_ID=otel-unified-stream-smoke-20260510-111318 BENCH_OTEL_EXPORTER=otlp-http BENCH_OTEL_EXPORTER_OTLP_ENDPOINT=http://otelop:4318/v1/traces WARMUP_STREAMS=1 INCLUDE_FRANKEN=0 INCLUDE_POC=0 ./bench/phase2/compare-small-select-streaming.sh`
- `docker compose run --rm -e BENCH_OTEL_RUN_ID=otel-unified-stream-smoke-20260510-111318 dev php tools/phase2/otelop-summary.php --run-id=otel-unified-stream-smoke-20260510-111318 --suite=small-select-streaming --limit=20000`
- `BENCH_OTEL_RUN_ID=otel-phase2-general-smoke-20260510-111652 BENCH_OTEL_EXPORTER=otlp-http BENCH_OTEL_EXPORTER_OTLP_ENDPOINT=http://otelop:4318/v1/traces ./bench/phase2/run.sh throughput-unary --duration=0.05 --warmup-calls=1`
- `docker compose run --rm -e BENCH_OTEL_RUN_ID=otel-phase2-general-smoke-20260510-111652 dev php tools/phase2/otelop-summary.php --run-id=otel-phase2-general-smoke-20260510-111652 --suite=throughput-unary --limit=20000`

Confirmed:

- Spanner DML unary shape emits comparable OTEL spans for php-grpc-lite and ext-grpc.
- Small select streaming emits comparable OTEL spans for php-grpc-lite and ext-grpc across 100B, 1KiB, 4KiB, and 10KiB payloads.
- `throughput-unary` emits OTEL spans via the generic Phase 2 runner path.
- php-grpc-lite spans include `grpc_lite.*` internal attributes; ext-grpc spans have the same outer RPC span model without internal php-grpc-lite attributes.
- OTLP export happens after measured spans are ended, so export overhead is outside span duration.

## Decision Log

- 2026-05-10: OpenTelemetry integration has two primary goals: application-side internal observability and replacing bespoke benchmark diagnostics with OTEL-based measurement.
- 2026-05-10: The benchmark measurement source should be common outer RPC spans, not php-grpc-lite-only C telemetry record spans.
- 2026-05-10: C extension should not own OTLP export responsibilities.
- 2026-05-10: For benchmark tooling, JSON/TSV can remain temporarily as compatibility output, but the primary comparison output for Spanner shape is now `otelop-summary.php`.

## Close Criteria

- Production telemetry design doc reflects the final responsibility boundary. Done.
- Benchmark runners can produce comparable OTEL spans for php-grpc-lite and ext-grpc. Done for common Phase 2 RPC helpers and Spanner compare runners.
- Spanner DML unary and small select streaming can be summarized from OTEL data. Done.
- Current synchronous `BenchTelemetry` exporter is removed or clearly replaced by the final span recorder. Done.
- Verification commands and results are recorded in this issue. Done.

## Close Summary

Closed on 2026-05-10. JSON/TSV output remains as compatibility output during migration, but Spanner shape and generic Phase 2 RPC smoke can now use OTEL span output as the primary comparison source.
