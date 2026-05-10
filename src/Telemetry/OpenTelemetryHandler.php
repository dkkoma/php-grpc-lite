<?php

declare(strict_types=1);

namespace GrpcLite\Telemetry;

final class OpenTelemetryHandler
{
    /**
     * @param array<string, mixed> $record
     */
    public function __invoke(array $record): void
    {
        if (!\class_exists('OpenTelemetry\\API\\Trace\\Span')) {
            return;
        }

        $span = \OpenTelemetry\API\Trace\Span::getCurrent();
        if (!\is_object($span)) {
            return;
        }

        $attributes = self::attributes($record);
        foreach ($attributes as $key => $value) {
            if (\method_exists($span, 'setAttribute')) {
                $span->setAttribute($key, $value);
            }
        }

        if (\method_exists($span, 'addEvent')) {
            $span->addEvent('grpc.client', $attributes);
        }
    }

    /**
     * @param array<string, mixed> $record
     * @return array<string, int|float|string|bool>
     */
    private static function attributes(array $record): array
    {
        $attributes = [
            'rpc.system' => 'grpc',
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
            $attributes['grpc_lite.' . $key] = \is_bool($value) ? $value : (int) $value;
        }
        foreach (($record['connection'] ?? []) as $key => $value) {
            $attributes['grpc_lite.connection_' . $key] = (bool) $value;
        }

        return $attributes;
    }
}
