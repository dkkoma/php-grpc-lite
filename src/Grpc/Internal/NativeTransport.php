<?php
declare(strict_types=1);

namespace Grpc\Internal;

/**
 * Thin PHP wrapper around the Phase 2 nghttp2 transport MVP extension.
 *
 * This is intentionally opt-in and benchmark-oriented. It supports the
 * insecure h2c path used by the controlled Go test-server benchmarks.
 */
final class NativeTransport
{
    /**
     * @param array<string, string> $headers
     * @return array{payloads: list<string>, grpc_status: int, http_status: int, trailers: array<string, list<string>>, raw: array<string, mixed>}
     */
    public static function unaryBatch(
        string $target,
        string $path,
        string $serializedRequest,
        array $headers,
        bool $compactResponseBuffer,
        bool $directResponsePayload,
    ): array {
        if (!function_exists('nghttp2_poc_unary_batch')) {
            throw new \RuntimeException('nghttp2_poc extension is not loaded');
        }

        [$host, $port] = self::splitTarget($target);
        $payloads = [];

        $result = \nghttp2_poc_unary_batch(
            $host,
            $port,
            $path,
            $serializedRequest,
            1,
            $headers,
            true,
            true,
            0,
            true,
            false,
            16 * 1024 * 1024,
            16 * 1024 * 1024,
            64 * 1024,
            true,
            false,
            static function (string $payload) use (&$payloads): null {
                $payloads[] = $payload;
                return null;
            },
            true,
            $compactResponseBuffer,
            65536,
            $directResponsePayload,
            false,
            0,
            0,
            false,
        );

        return [
            'payloads' => $payloads,
            'grpc_status' => (int) ($result['grpc_status'] ?? -1),
            'http_status' => (int) ($result['http_status'] ?? 0),
            'trailers' => self::extractTrailers($result),
            'raw' => $result,
        ];
    }

    /**
     * @param array<string, mixed> $result
     * @return array<string, list<string>>
     */
    private static function extractTrailers(array $result): array
    {
        $trailers = [
            'grpc-status' => [(string) ((int) ($result['grpc_status'] ?? -1))],
        ];

        foreach ([
            'server_stats_handler_start_ns',
            'server_stats_handler_end_ns',
            'server_stats_in_payload_ns',
            'server_stats_out_header_ns',
            'server_stats_out_payload_ns',
            'server_stats_first_out_payload_ns',
            'server_stats_last_out_payload_ns',
            'server_stats_out_payload_count',
            'server_stats_out_payload_bytes',
            'server_stats_out_payload_wire_bytes',
            'server_stats_out_payload_compressed_bytes',
        ] as $field) {
            $value = self::firstScalarValue($result[$field] ?? null);
            if ($value === null) {
                continue;
            }
            $trailers['x-bench-' . str_replace('_', '-', $field)] = [(string) $value];
        }

        return $trailers;
    }

    private static function firstScalarValue(mixed $value): int|string|null
    {
        if (is_int($value) || is_string($value)) {
            return $value;
        }
        if (!is_array($value) || $value === []) {
            return null;
        }

        $first = reset($value);
        return is_int($first) || is_string($first) ? $first : null;
    }

    /** @return array{0: string, 1: int} */
    private static function splitTarget(string $target): array
    {
        $pos = strrpos($target, ':');
        if ($pos === false) {
            return [$target, 80];
        }

        return [substr($target, 0, $pos), (int) substr($target, $pos + 1)];
    }
}
