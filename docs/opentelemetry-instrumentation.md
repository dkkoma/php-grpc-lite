# OpenTelemetry / telemetry instrumentation design

php-grpc-lite の計測は、RPC実行中の低レベル観測点を vendor-neutral な telemetry record としてC拡張からPHP callbackへ渡す。C拡張は OpenTelemetry / Datadog exporter を直接持たない。

## 目的

- Laravel などのPHPアプリケーションで作られた active span に gRPC client の内部計測を接続する。
- unary / server streaming の transport 内訳を、遅い1 callを掘れる粒度で可視化する。
- OpenTelemetry SDK、Datadog ddtrace、ローカル開発用JSON/otelop変換を同じrecord境界から扱う。

## Responsibility boundary

| layer | 責務 |
|---|---|
| C extension | RPCごとの計測recordを作り、登録済みPHP handlerへ渡す |
| PHP adapter | recordをOpenTelemetry span event/attribute、ddtrace tag/metric、または開発用出力へ変換する |
| Observability SDK / Collector | sampling、batch、flush、export、retry、shutdownを担当する |

C拡張は exporter failure、SDK failure、handler exception をRPC結果に影響させない。handlerのrequest lifecycle cleanupは行うが、OpenTelemetry SDKのflush/exportはSDK側に委譲する。

## Application context connection

アプリケーション側のspanと接続する境界は gRPC metadata の W3C Trace Context とする。

- PHPアプリケーションは active OpenTelemetry context を `traceparent` / `tracestate` metadata として注入する。
- php-grpc-lite の telemetry record は request metadata の `traceparent` を必要に応じて含める。
- 下流gRPC serverにも同じ metadata を送るため、distributed traceとしてもつながる。

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

## Runtime configuration

| setting | default | 意味 |
|---|---:|---|
| `grpc_lite.telemetry_enabled` | `0` | C拡張からPHP handlerへのrecord送出を有効化 |
| `grpc_lite.telemetry_detail_level` | `rpc` | `rpc`, `phase`, `debug`。詳細event追加時の粒度選択に使う |

sampling、span event上限、batch size、export endpointはextensionでは持たない。OpenTelemetry SDK / Datadog tracer / Collector / otelop側の責務とする。

## PHP handler API

C拡張は `grpc_lite_set_telemetry_handler(?callable $handler): void` を公開する。通常は `GrpcLite\Telemetry\Telemetry::setHandler()` 経由で設定する。このhelperはhandler登録時に `grpc_lite.telemetry_enabled=1`、解除時に `0` へ切り替える。

```php
use GrpcLite\Telemetry\Telemetry;
use GrpcLite\Telemetry\OpenTelemetryHandler;

Telemetry::setHandler(new OpenTelemetryHandler());
```

Datadog ddtraceへ接続する場合は active span にtag/metricを付与する。

```php
use GrpcLite\Telemetry\DdTraceHandler;
use GrpcLite\Telemetry\Telemetry;

Telemetry::setHandler(new DdTraceHandler());
```

## Record model

1 RPCにつき1 recordを生成する。payloadやmetadata valueは原則含めず、byte数やstatusなどの診断属性だけを渡す。

| field | 内容 |
|---|---|
| `kind` | `unary` などのcall種別 |
| `rpc_system`, `rpc_service`, `rpc_method` | gRPC method識別子 |
| `grpc_status_code`, `http_status_code` | RPC結果 |
| `start_unix_nanos`, `duration_us` | wall time |
| `timings` | `setup_us`, `submit_us`, `initial_send_us`, `recv_loop_us` |
| `sizes` | request/response body bytes, sent/received bytes |
| `http2` | stream id, frame count, RST_STREAM観測 |
| `connection` | reuse, persistent reuse, dead/draining |
| `traceparent` | request metadataに存在する場合だけコピー |

## Bench diagnostics migration

ベンチ計測の一次ソースはOpenTelemetry spanに寄せる。php-grpc-lite、公式 ext-grpc、franken-go はすべてPHP runner側の共通RPC spanでwall timeを測る。php-grpc-liteだけはC拡張のtelemetry recordをactive RPC spanへ追加属性として付与する。

既存のJSON/TSVは移行期間の互換出力として残せるが、性能判断の主出力はOTEL backendからのquery結果にする。Spanner shape系では `tools/phase2/otelop-summary.php` が `benchmark.run_id` でspanを抽出し、suite / measurement / shape / implementation別にp50/p99を集計する。

| 既存計測 | telemetry表現 | 方針 |
|---|---|---|
| `total_us`, `setup_us`, `submit_us`, `initial_send_us`, `recv_loop_us` | `duration_us` + `timings` | unary / server streaming 共通のphase計測にする |
| `bytes_sent`, `bytes_received`, request/response payload bytes | `sizes` | payloadそのものは出さず、byte数だけ出す |
| `sent_frames`, `recv_frames`, stream error | `http2` | frame単位eventはdefaultでは出さない |
| connection reused / dead / draining / GOAWAY | `connection` + future events | channel lifecycle判断に必要 |
| HTTP status / gRPC status / RST_STREAM | record status + `http2` | RPC結果とtransport異常を分ける |
| initial/trailing metadata maps | 原則出さない | key数/byte数だけにする。metadata valueは認証情報を含み得るため出さない |

## Local development with otelop

otelopへ送る場合もproduction pathではC拡張はOTLPを直接送らない。PHP側でOpenTelemetry SDK exporterを設定し、`OpenTelemetryHandler` から active span へevent/attributeを追加する。OTLP endpointはSDK設定で `http://otelop:4318` または `http://localhost:4318` を指定する。

Phase 2 ベンチだけは、アプリケーションspanが存在しない状態でも比較できるように `tools/phase2/BenchTelemetry.php` のspan recorderを使う。これは全実装共通のRPC spanを記録し、測定終了後にまとめて `otelop` のOTLP/HTTP receiverへ送る。php-grpc-lite内部recordはactive RPC spanへ追加されるため、export overheadはRPC span durationに含まれない。

```bash
docker compose up -d otelop

BENCH_OTEL_EXPORTER=otlp-http \
BENCH_OTEL_RUN_ID=local-otel \
BENCH_OTEL_EXPORTER_OTLP_ENDPOINT=http://otelop:4318/v1/traces \
./bench/phase2/compare-spanner-dml-unary-shape.sh

docker compose run --rm -e BENCH_OTEL_RUN_ID=local-otel dev php \
  tools/phase2/otelop-summary.php \
  --run-id=local-otel \
  --suite=spanner-dml-unary-shape
```

ブラウザでは `http://localhost:4319` を開く。spanには `benchmark.suite`、`benchmark.implementation`、`benchmark.measurement`、payload size、stream count、transport、`grpc_lite.*` のtiming / size / HTTP/2 / connection属性が入る。
