<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tools\Phase2;

use GrpcLite\Telemetry\Telemetry;

final class BenchTelemetry
{
    /** @var array<string, int|float|string|bool|null> */
    private array $context = [];

    /** @var list<array<string, mixed>> */
    private array $spans = [];

    /** @var list<int> */
    private array $activeSpanStack = [];

    private readonly string $runId;

    private function __construct(
        private readonly string $endpoint,
        private readonly string $suite,
        private readonly string $implementation,
    ) {
        $this->runId = getenv('BENCH_OTEL_RUN_ID') ?: (getenv('BENCH_TAG') ?: date('Ymd-His'));
    }

    public static function fromEnvironment(string $suite, string $implementation): ?self
    {
        $exporterName = getenv('BENCH_OTEL_EXPORTER') ?: '';
        if ($exporterName !== '' && !in_array($exporterName, ['otlp-http', 'otlp'], true)) {
            return null;
        }

        $endpoint = getenv('BENCH_OTEL_EXPORTER_OTLP_ENDPOINT') ?: getenv('OTEL_EXPORTER_OTLP_TRACES_ENDPOINT') ?: getenv('OTEL_EXPORTER_OTLP_ENDPOINT') ?: '';
        if ($endpoint === '') {
            return null;
        }

        $recorder = new self(self::normalizeEndpoint($endpoint), $suite, $implementation);
        if (function_exists('grpc_lite_set_telemetry_handler')) {
            Telemetry::setHandler([$recorder, 'recordInternalTelemetry']);
        }

        return $recorder;
    }

    public function shutdown(): void
    {
        if (function_exists('grpc_lite_set_telemetry_handler')) {
            Telemetry::setHandler(null);
        }
        $this->export();
    }

    /** @param array<string, int|float|string|bool|null> $context */
    public function setContext(string $measurement, array $context = []): void
    {
        $this->context = [
            'benchmark.run_id' => $this->runId,
            'benchmark.suite' => $this->suite,
            'benchmark.implementation' => $this->implementation,
            'benchmark.measurement' => $measurement,
        ] + $context;
    }

    /**
     * @template T
     * @param array<string, int|float|string|bool|null> $attributes
     * @param callable(): T $callback
     * @return T
     */
    public function measureRpc(string $name, array $attributes, callable $callback): mixed
    {
        $spanIndex = $this->startSpan($name, ['rpc.system' => 'grpc'] + $attributes);
        try {
            $result = $callback();
            $this->finishSpan($spanIndex, 1);
            return $result;
        } catch (\Throwable $throwable) {
            $this->finishSpan($spanIndex, 2, [
                'exception.type' => $throwable::class,
                'exception.message' => $throwable->getMessage(),
            ]);
            throw $throwable;
        }
    }

    /** @param array<string, mixed> $record */
    public function recordInternalTelemetry(array $record): void
    {
        $spanIndex = $this->activeSpanStack[array_key_last($this->activeSpanStack)] ?? null;
        if ($spanIndex === null || !isset($this->spans[$spanIndex])) {
            return;
        }

        foreach (self::recordAttributes($record) as $key => $value) {
            $this->spans[$spanIndex]['attributes'][$key] = $value;
        }
        $this->spans[$spanIndex]['status_code'] = ((int) ($record['grpc_status_code'] ?? 0)) === 0 ? 1 : 2;
    }

    /** @param array<string, int|float|string|bool|null> $attributes */
    private function startSpan(string $name, array $attributes): int
    {
        $now = hrtime(true);
        $index = count($this->spans);
        $this->spans[] = [
            'trace_id' => bin2hex(random_bytes(16)),
            'span_id' => bin2hex(random_bytes(8)),
            'name' => $name,
            'kind' => 3,
            'start_unix_nanos' => self::unixTimeNanos(),
            'start_monotonic_ns' => $now,
            'end_unix_nanos' => null,
            'attributes' => self::cleanAttributes($this->context + $attributes),
            'status_code' => 1,
        ];
        $this->activeSpanStack[] = $index;

        return $index;
    }

    /** @param array<string, int|float|string|bool|null> $attributes */
    private function finishSpan(int $spanIndex, int $statusCode, array $attributes = []): void
    {
        $span = &$this->spans[$spanIndex];
        $elapsedNs = hrtime(true) - (int) $span['start_monotonic_ns'];
        $span['end_unix_nanos'] = (string) (((int) $span['start_unix_nanos']) + $elapsedNs);
        $span['status_code'] = $statusCode;
        foreach (self::cleanAttributes($attributes) as $key => $value) {
            $span['attributes'][$key] = $value;
        }

        $stackKey = array_search($spanIndex, $this->activeSpanStack, true);
        if ($stackKey !== false) {
            array_splice($this->activeSpanStack, (int) $stackKey, 1);
        }
    }

    private function export(): void
    {
        if ($this->spans === []) {
            return;
        }

        foreach (array_chunk($this->spans, 500) as $chunk) {
            $payload = $this->buildPayload($chunk);
            $context = stream_context_create([
                'http' => [
                    'method' => 'POST',
                    'header' => "Content-Type: application/json\r\n",
                    'content' => json_encode($payload, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE),
                    'timeout' => 5.0,
                    'ignore_errors' => true,
                ],
            ]);
            @file_get_contents($this->endpoint, false, $context);
        }
    }

    private static function normalizeEndpoint(string $endpoint): string
    {
        $endpoint = rtrim($endpoint, '/');
        if (str_ends_with($endpoint, '/v1/traces')) {
            return $endpoint;
        }

        return $endpoint . '/v1/traces';
    }

    /** @param list<array<string, mixed>> $spans */
    private function buildPayload(array $spans): array
    {
        $otelSpans = [];
        foreach ($spans as $span) {
            if ($span['end_unix_nanos'] === null) {
                continue;
            }
            $otelSpans[] = [
                'traceId' => $span['trace_id'],
                'spanId' => $span['span_id'],
                'name' => $span['name'],
                'kind' => $span['kind'],
                'startTimeUnixNano' => (string) $span['start_unix_nanos'],
                'endTimeUnixNano' => (string) $span['end_unix_nanos'],
                'attributes' => self::otelAttributes($span['attributes']),
                'status' => ['code' => $span['status_code']],
            ];
        }

        return [
            'resourceSpans' => [[
                'resource' => ['attributes' => self::otelAttributes([
                    'service.name' => 'php-grpc-lite-bench',
                    'benchmark.run_id' => $this->runId,
                ])],
                'scopeSpans' => [[
                    'scope' => ['name' => 'php-grpc-lite-bench'],
                    'spans' => $otelSpans,
                ]],
            ]],
        ];
    }

    /** @param array<string, mixed> $record */
    private static function recordAttributes(array $record): array
    {
        $attributes = [
            'rpc.service' => (string) ($record['rpc_service'] ?? ''),
            'rpc.method' => (string) ($record['rpc_method'] ?? ''),
            'network.protocol.name' => (string) ($record['network_protocol_name'] ?? 'http'),
            'network.protocol.version' => (string) ($record['network_protocol_version'] ?? '2'),
            'grpc.status_code' => (int) ($record['grpc_status_code'] ?? 0),
            'http.response.status_code' => (int) ($record['http_status_code'] ?? 0),
            'grpc_lite.backend' => (string) ($record['backend'] ?? 'http2'),
            'grpc_lite.duration_us' => (int) ($record['duration_us'] ?? 0),
        ];

        foreach (($record['timings'] ?? []) as $key => $value) {
            $attributes['grpc_lite.' . $key] = (int) $value;
        }
        foreach (($record['sizes'] ?? []) as $key => $value) {
            $attributes['grpc_lite.' . $key] = (int) $value;
        }
        foreach (($record['http2'] ?? []) as $key => $value) {
            $attributes['grpc_lite.' . $key] = is_bool($value) ? $value : (int) $value;
        }
        foreach (($record['connection'] ?? []) as $key => $value) {
            $attributes['grpc_lite.connection_' . $key] = (bool) $value;
        }
        if (isset($record['message_count'])) {
            $attributes['grpc_lite.message_count'] = (int) $record['message_count'];
        }

        return $attributes;
    }

    /** @param array<string, mixed> $attributes */
    private static function cleanAttributes(array $attributes): array
    {
        $clean = [];
        foreach ($attributes as $key => $value) {
            if ($value === null || is_array($value) || is_object($value)) {
                continue;
            }
            $clean[$key] = $value;
        }
        return $clean;
    }

    /** @param array<string, int|float|string|bool|null> $attributes */
    private static function otelAttributes(array $attributes): array
    {
        $otelAttributes = [];
        foreach ($attributes as $key => $value) {
            if ($value === null) {
                continue;
            }
            $otelAttributes[] = ['key' => $key, 'value' => self::otelValue($value)];
        }

        return $otelAttributes;
    }

    /** @return array<string, string|float|bool> */
    private static function otelValue(int|float|string|bool $value): array
    {
        return match (true) {
            is_bool($value) => ['boolValue' => $value],
            is_int($value) => ['intValue' => (string) $value],
            is_float($value) => ['doubleValue' => $value],
            default => ['stringValue' => $value],
        };
    }

    private static function unixTimeNanos(): string
    {
        return (string) ((int) (microtime(true) * 1_000_000_000));
    }
}
