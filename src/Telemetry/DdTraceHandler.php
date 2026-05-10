<?php

declare(strict_types=1);

namespace GrpcLite\Telemetry;

final class DdTraceHandler
{
    /**
     * @param array<string, mixed> $record
     */
    public function __invoke(array $record): void
    {
        if (!\function_exists('DDTrace\\active_span')) {
            return;
        }

        $span = \DDTrace\active_span();
        if (!\is_object($span)) {
            return;
        }

        $meta = [
            'rpc.system' => 'grpc',
            'rpc.service' => (string) ($record['rpc_service'] ?? ''),
            'rpc.method' => (string) ($record['rpc_method'] ?? ''),
            'grpc_lite.backend' => (string) ($record['backend'] ?? 'http2'),
        ];
        $metrics = [
            'grpc.status_code' => (float) ($record['grpc_status_code'] ?? 0),
            'http.response.status_code' => (float) ($record['http_status_code'] ?? 0),
            'grpc_lite.duration_us' => (float) ($record['duration_us'] ?? 0),
        ];

        foreach (($record['timings'] ?? []) as $key => $value) {
            $metrics['grpc_lite.' . $key] = (float) $value;
        }
        foreach (($record['sizes'] ?? []) as $key => $value) {
            $metrics['grpc_lite.' . $key] = (float) $value;
        }
        foreach (($record['http2'] ?? []) as $key => $value) {
            $metrics['grpc_lite.' . $key] = \is_bool($value) ? ($value ? 1.0 : 0.0) : (float) $value;
        }
        foreach (($record['connection'] ?? []) as $key => $value) {
            $metrics['grpc_lite.connection_' . $key] = $value ? 1.0 : 0.0;
        }

        if (\property_exists($span, 'meta') && \is_array($span->meta)) {
            foreach ($meta as $key => $value) {
                $span->meta[$key] = $value;
            }
        }
        if (\property_exists($span, 'metrics') && \is_array($span->metrics)) {
            foreach ($metrics as $key => $value) {
                $span->metrics[$key] = $value;
            }
        }
    }
}
