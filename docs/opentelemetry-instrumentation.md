# OpenTelemetry integration

php-grpc-lite のOpenTelemetry対応は、アプリケーション側の trace context 伝播と、ベンチrunner側のOTLP exportに限定する。

C拡張のhot pathからPHP callbackへ内部計測recordを送る仕組みは持たない。RPCの実行時間を歪めやすく、ext-grpcとの比較境界も揃わないため、production runtimeの内部観測としては採用しない。

## Application context connection

アプリケーション側のspanと接続する境界は gRPC metadata の W3C Trace Context とする。

- PHPアプリケーションは active OpenTelemetry context を `traceparent` / `tracestate` metadata として注入する。
- php-grpc-lite は受け取った metadata を通常のgRPC request metadataとして送る。
- C拡張はOpenTelemetry SDK、Datadog exporter、Collector、sampling、flush、retryを直接扱わない。

php-grpc-lite は `GrpcLite\OpenTelemetry\TraceContextMetadata` を提供する。OpenTelemetry API が入っていない環境では no-op になる。

```php
use GrpcLite\OpenTelemetry\TraceContextMetadata;

$client = new ExampleGrpcClient($target, [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
    'update_metadata' => TraceContextMetadata::updateMetadataCallback(),
]);
```

既に `Grpc\Interceptor` を使っている場合は `TraceContextInterceptor` を channel に追加できる。

```php
use Grpc\Interceptor;
use GrpcLite\OpenTelemetry\TraceContextInterceptor;

$channel = new Grpc\Channel($target, [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
]);

$client = new ExampleGrpcClient(
    $target,
    ['credentials' => Grpc\ChannelCredentials::createInsecure()],
    Interceptor::intercept($channel, new TraceContextInterceptor()),
);
```

## Benchmark OTEL export

Benchmark ベンチは任意で `otelop` へOTLP/HTTP exportできる。計測境界はphp-grpc-lite、公式 ext-grpc、franken-goで共通のPHP runner外側境界に揃える。

- RPC開始直前に `hrtime(true)` を取る。
- RPC完了直後に `hrtime(true)` を取る。
- RPC後に `BenchTelemetry::recordRpcSpan()` でspanを組み立てる。
- OTLP exportは測定終了後にbatch送信する。

この方式では、span生成、trace id生成、attribute構築、OTLP exportのcostをRPC durationへ含めない。PHP runner外側のwall-clock境界をspan durationとして記録し、otelop上の値を一次比較値にする。

spanには `benchmark.run_id`、`benchmark.suite`、`benchmark.implementation`、`benchmark.measurement`、shape属性、`rpc.system` が入る。C拡張内部の `grpc_lite.*` timing / size / HTTP/2属性は入れない。

```bash
docker compose up -d otelop

BENCH_OTEL_EXPORTER=otlp-http \
BENCH_OTEL_RUN_ID=local-otel \
BENCH_OTEL_EXPORTER_OTLP_ENDPOINT=http://otelop:4318/v1/traces \
./bench/compare-spanner-dml-unary-shape.sh

docker compose run --rm -e BENCH_OTEL_RUN_ID=local-otel dev php \
  tools/benchmark/otelop-summary.php \
  --run-id=local-otel \
  --suite=spanner-dml-unary-shape
```

ブラウザでは `http://localhost:4319` を開く。OTLP/HTTP endpointはcompose内から `http://otelop:4318/v1/traces`、ホストから直接送る場合は `http://localhost:4318/v1/traces` を使う。
