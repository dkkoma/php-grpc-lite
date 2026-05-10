<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tools\Phase2;

use GrpcLite\Telemetry\Telemetry;

final class BenchTelemetry
{
    /** @var array<string, int|float|string|bool|null> */
    private array $context = [];

    private function __construct(
        private readonly string $endpoint,
        private readonly string $suite,
        private readonly string $implementation,
    ) {
    }

    public static function fromEnvironment(string $suite, string $implementation): ?self
    {
        if (!function_exists('grpc_lite_set_telemetry_handler')) {
            return null;
        }

        $exporterName = getenv('BENCH_OTEL_EXPORTER') ?: '';
        if ($exporterName !== '' && !in_array($exporterName, ['otlp-http', 'otlp'], true)) {
            return null;
        }

        $endpoint = getenv('BENCH_OTEL_EXPORTER_OTLP_ENDPOINT') ?: getenv('OTEL_EXPORTER_OTLP_TRACES_ENDPOINT') ?: getenv('OTEL_EXPORTER_OTLP_ENDPOINT') ?: '';
        if ($endpoint === '') {
            return null;
        }

        $exporter = new self(self::normalizeEndpoint($endpoint), $suite, $implementation);
        Telemetry::setHandler($exporter);

        return $exporter;
    }

    public function shutdown(): void
    {
        Telemetry::setHandler(null);
    }

    /** @param array<string, int|float|string|bool|null> $context */
    public function setContext(string $measurement, array $context = []): void
    {
        $this->context = ['benchmark.measurement' => $measurement] + $context;
    }

    /** @param array<string, mixed> $record */
    public function __invoke(array $record): void
    {
        $payload = $this->buildPayload($record);
        $context = stream_context_create([
            'http' => [
                'method' => 'POST',
                'header' => "Content-Type: application/json\r\n",
                'content' => json_encode($payload, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE),
                'timeout' => 0.2,
                'ignore_errors' => true,
            ],
        ]);
        @file_get_contents($this->endpoint, false, $context);
    }

    private static function normalizeEndpoint(string $endpoint): string
    {
        $endpoint = rtrim($endpoint, '/');
        if (str_ends_with($endpoint, '/v1/traces')) {
            return $endpoint;
        }

        return $endpoint . '/v1/traces';
    }

    /** @param array<string, mixed> $record */
    private function buildPayload(array $record): array
    {
        [$traceId, $parentSpanId] = self::traceContext($record['traceparent'] ?? null);
        $spanId = bin2hex(random_bytes(8));
        $start = (string) ($record['start_unix_nanos'] ?? (string) (time() * 1_000_000_000));
        $durationUs = (int) ($record['duration_us'] ?? 0);
        $end = (string) (((int) $start) + ($durationUs * 1000));
        $attributes = self::attributes($record, [
            'service.name' => 'php-grpc-lite-bench',
            'benchmark.suite' => $this->suite,
            'benchmark.implementation' => $this->implementation,
        ] + $this->context);

        $span = [
            'traceId' => $traceId,
            'spanId' => $spanId,
            'name' => sprintf(
                'bench %s %s/%s',
                (string) ($record['kind'] ?? 'rpc'),
                (string) ($record['rpc_service'] ?? ''),
                (string) ($record['rpc_method'] ?? ''),
            ),
            'kind' => 3,
            'startTimeUnixNano' => $start,
            'endTimeUnixNano' => $end,
            'attributes' => $attributes,
            'status' => ['code' => ((int) ($record['grpc_status_code'] ?? 0)) === 0 ? 1 : 2],
        ];
        if ($parentSpanId !== null) {
            $span['parentSpanId'] = $parentSpanId;
        }

        return [
            'resourceSpans' => [[
                'resource' => ['attributes' => self::attributes([], ['service.name' => 'php-grpc-lite-bench'])],
                'scopeSpans' => [[
                    'scope' => ['name' => 'php-grpc-lite-bench'],
                    'spans' => [$span],
                ]],
            ]],
        ];
    }

    /**
     * @param array<string, mixed> $record
     * @param array<string, int|float|string|bool|null> $extra
     * @return list<array{key: string, value: array<string, string|int|float|bool>}>
     */
    private static function attributes(array $record, array $extra = []): array
    {
        $flat = $extra + [
            'rpc.system' => (string) ($record['rpc_system'] ?? 'grpc'),
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
            $flat['grpc_lite.' . $key] = (int) $value;
        }
        foreach (($record['sizes'] ?? []) as $key => $value) {
            $flat['grpc_lite.' . $key] = (int) $value;
        }
        foreach (($record['http2'] ?? []) as $key => $value) {
            $flat['grpc_lite.' . $key] = is_bool($value) ? $value : (int) $value;
        }
        foreach (($record['connection'] ?? []) as $key => $value) {
            $flat['grpc_lite.connection_' . $key] = (bool) $value;
        }
        if (isset($record['message_count'])) {
            $flat['grpc_lite.message_count'] = (int) $record['message_count'];
        }

        $attributes = [];
        foreach ($flat as $key => $value) {
            if ($value === null) {
                continue;
            }
            $attributes[] = ['key' => $key, 'value' => self::otelValue($value)];
        }

        return $attributes;
    }

    /** @return array<string, string|int|float|bool> */
    private static function otelValue(int|float|string|bool $value): array
    {
        return match (true) {
            is_bool($value) => ['boolValue' => $value],
            is_int($value) => ['intValue' => (string) $value],
            is_float($value) => ['doubleValue' => $value],
            default => ['stringValue' => $value],
        };
    }

    /** @return array{0: string, 1: string|null} */
    private static function traceContext(mixed $traceparent): array
    {
        if (is_string($traceparent)
            && preg_match('/^[0-9a-f]{2}-([0-9a-f]{32})-([0-9a-f]{16})-[0-9a-f]{2}$/', $traceparent, $matches) === 1
        ) {
            return [$matches[1], $matches[2]];
        }

        return [bin2hex(random_bytes(16)), null];
    }
}
