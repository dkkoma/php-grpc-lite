# OpenTelemetryをライブラリ内部観測とベンチ計測の共通基盤にする

- Status: Open
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

1. Replace bench-specific synchronous OTLP exporter with a bench span recorder that uses a common OTEL pipeline.
2. Add shared benchmark instrumentation helpers for measurement spans and per-RPC spans.
3. Make unary and server-streaming benchmark helpers create common outer RPC spans for all implementations.
4. Attach php-grpc-lite C telemetry records to the active RPC span via `OpenTelemetryHandler`.
5. Add otelop/OTEL query CLI to aggregate p50/p99/throughput from spans.
6. Migrate Spanner DML unary shape and small select streaming first.
7. Decide how much of legacy JSON/TSV output remains as derived/compat output.

## Progress

- 2026-05-10: Current `BenchTelemetry` can export php-grpc-lite internal records to otelop, but this is not the desired final model because it is synchronous and not shared by ext-grpc.
- 2026-05-10: Spanner DML unary and small select streaming were run with the current exporter; results confirmed otelop visibility but also showed that TSV wall time includes exporter overhead.

## Verification

Pending.

Expected checks:

- `docker compose up -d otelop`
- Spanner DML unary shape emits measurement/rpc spans for php-grpc-lite and ext-grpc.
- Small select streaming emits measurement/rpc spans for php-grpc-lite and ext-grpc.
- otelop GraphQL query can compute p50/p99 grouped by benchmark measurement and implementation.
- php-grpc-lite spans include `grpc_lite.*` attributes; ext-grpc spans do not claim internal attributes.
- Export overhead is not included in RPC span duration when using asynchronous/SDK exporter path, or is clearly outside measured span boundary.

## Decision Log

- 2026-05-10: OpenTelemetry integration has two primary goals: application-side internal observability and replacing bespoke benchmark diagnostics with OTEL-based measurement.
- 2026-05-10: The benchmark measurement source should be common outer RPC spans, not php-grpc-lite-only C telemetry record spans.
- 2026-05-10: C extension should not own OTLP export responsibilities.

## Close Criteria

- Production telemetry design doc reflects the final responsibility boundary.
- Benchmark runners can produce comparable OTEL spans for php-grpc-lite and ext-grpc.
- Spanner DML unary and small select streaming can be summarized from OTEL data.
- Current synchronous `BenchTelemetry` exporter is removed or clearly replaced by the final span recorder.
- Verification commands and results are recorded in this issue.
