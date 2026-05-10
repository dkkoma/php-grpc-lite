<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tools\Phase2;

final class BenchTelemetry
{
    /** @var array<string, int|float|string|bool|null> */
    private array $context = [];

    /** @var list<array<string, mixed>> */
    private array $spans = [];

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

        return new self(self::normalizeEndpoint($endpoint), $suite, $implementation);
    }


    public static function requiredFromEnvironment(string $suite, string $implementation): self
    {
        $telemetry = self::fromEnvironment($suite, $implementation);
        if ($telemetry === null) {
            throw new \RuntimeException('OTEL endpoint is required. Set BENCH_OTEL_EXPORTER_OTLP_ENDPOINT or OTEL_EXPORTER_OTLP_TRACES_ENDPOINT.');
        }

        return $telemetry;
    }

    public function shutdown(): void
    {
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
     * @param array<string, int|float|string|bool|null> $attributes
     */
    public function recordRpcSpan(string $name, int $startNs, int $endNs, array $attributes = [], int $statusCode = 1): void
    {
        if ($endNs < $startNs) {
            return;
        }

        $startUnixNanos = self::unixTimeNanosFromMonotonic($startNs);
        $this->spans[] = [
            'trace_id' => bin2hex(random_bytes(16)),
            'span_id' => bin2hex(random_bytes(8)),
            'name' => $name,
            'kind' => 3,
            'start_unix_nanos' => $startUnixNanos,
            'end_unix_nanos' => (string) (((int) $startUnixNanos) + ($endNs - $startNs)),
            'attributes' => self::cleanAttributes($this->context + ['rpc.system' => 'grpc'] + $attributes),
            'status_code' => $statusCode,
        ];
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

    private static function unixTimeNanosFromMonotonic(int $monotonicNs): string
    {
        $nowUnixNs = (int) (microtime(true) * 1_000_000_000);
        $nowMonotonicNs = hrtime(true);

        return (string) ($nowUnixNs - ($nowMonotonicNs - $monotonicNs));
    }
}
