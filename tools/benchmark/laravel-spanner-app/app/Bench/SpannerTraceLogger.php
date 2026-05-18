<?php

declare(strict_types=1);

namespace BenchApp\Bench;

use Psr\Log\AbstractLogger;

final class SpannerTraceLogger extends AbstractLogger
{
    /**
     * @param mixed $level
     * @param string|\Stringable $message
     * @param array<string, mixed> $context
     */
    public function log($level, string|\Stringable $message, array $context = []): void
    {
        if (!SpannerTraceRecorder::enabled()) {
            return;
        }

        $decoded = json_decode((string) $message, true);
        if (!is_array($decoded)) {
            SpannerTraceRecorder::record('gax.log', [
                'level' => (string) $level,
                'message' => (string) $message,
            ]);
            return;
        }

        $payload = isset($decoded['jsonPayload']) && is_array($decoded['jsonPayload'])
            ? $decoded['jsonPayload']
            : [];

        if (array_key_exists('request.method', $payload) || array_key_exists('request.payload', $payload)) {
            SpannerTraceRecorder::record('rpc.request', [
                'level' => (string) $level,
                'rpc_request_id' => $decoded['requestId'] ?? null,
                'rpc_name' => $decoded['rpcName'] ?? null,
                'retry_attempt' => $payload['retryAttempt'] ?? null,
                'request_headers' => self::safeHeaders($payload['request.headers'] ?? null),
                'request_payload' => $payload['request.payload'] ?? null,
            ]);
            return;
        }

        if (array_key_exists('response.status', $payload) || array_key_exists('latencyMillis', $payload)) {
            SpannerTraceRecorder::record('rpc.response', [
                'level' => (string) $level,
                'rpc_request_id' => $decoded['requestId'] ?? null,
                'rpc_name' => $decoded['rpcName'] ?? null,
                'status' => $payload['response.status'] ?? null,
                'latency_ms' => $payload['latencyMillis'] ?? null,
                'response_headers' => self::safeHeaders($payload['response.headers'] ?? null),
            ]);
            return;
        }

        SpannerTraceRecorder::record('gax.log', [
            'level' => (string) $level,
            'payload' => $payload,
        ]);
    }

    /**
     * @param mixed $headers
     * @return mixed
     */
    private static function safeHeaders(mixed $headers): mixed
    {
        if (!is_array($headers)) {
            return $headers;
        }
        foreach ($headers as $key => $_value) {
            if (is_string($key) && strcasecmp($key, 'authorization') === 0) {
                $headers[$key] = '[redacted]';
            }
        }
        return $headers;
    }
}
