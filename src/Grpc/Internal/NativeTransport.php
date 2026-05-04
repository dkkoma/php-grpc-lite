<?php
declare(strict_types=1);

namespace Grpc\Internal;

/**
 * Thin PHP wrapper around the Phase 2 nghttp2 transport MVP extension.
 *
 * This is benchmark-oriented glue for the current MVP extension. It supports
 * production packaging and true streaming resources are still release gates for
 * native default. Unary native channels support request-crossing process/thread
 * local persistence in the C extension.
 */
final class NativeTransport
{
    /**
     * @param array<string, list<string>> $headers
     * @return array{payloads: list<string>, grpc_status: int, details: string, http_status: int, headers: array<string, list<string>>, trailers: array<string, list<string>>, raw: array<string, mixed>}
     */
    public static function unaryBatch(
        string $target,
        string $path,
        string $serializedRequest,
        array $headers,
        bool $compactResponseBuffer,
        bool $directResponsePayload,
        int $timeoutMicros = 0,
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
            $timeoutMicros,
        );

        [$grpcStatus, $details] = self::normalizeStatus($result);

        return [
            'payloads' => $payloads,
            'grpc_status' => $grpcStatus,
            'details' => $details,
            'http_status' => (int) ($result['http_status'] ?? 0),
            'headers' => self::extractInitialMetadata($result),
            'trailers' => self::extractTrailers($result, $grpcStatus, $details),
            'raw' => $result,
        ];
    }

    /**
     * @param array<string, list<string>> $headers
     * @return array{payloads: list<string>, grpc_status: int, details: string, http_status: int, headers: array<string, list<string>>, trailers: array<string, list<string>>, raw: array<string, mixed>}
     */
    public static function unarySimple(
        string $target,
        string $path,
        string $serializedRequest,
        array $headers,
        int $timeoutMicros = 0,
        ?\Grpc\ChannelCredentials $credentials = null,
    ): array {
        if (!function_exists('nghttp2_poc_persistent_channel_unary')) {
            throw new \RuntimeException('nghttp2_poc persistent channel API is not loaded');
        }

        [$host, $port] = self::splitTarget($target);
        $framedRequest = "\0" . pack('N', strlen($serializedRequest)) . $serializedRequest;
        $key = self::channelKey($host, $port, $credentials);
        try {
            $useTls = $credentials !== null && !$credentials->isInsecure();
            $result = \nghttp2_poc_persistent_channel_unary(
                $key,
                $host,
                $port,
                $path,
                $framedRequest,
                $headers,
                $timeoutMicros,
                $useTls,
                $credentials?->rootCerts,
                $credentials?->certChain,
                $credentials?->privateKey,
            );
        } catch (\Throwable $e) {
            throw $e instanceof \RuntimeException ? $e : new \RuntimeException($e->getMessage(), 0, $e);
        }

        [$grpcStatus, $details] = self::normalizeStatus($result);
        $payloads = [];
        $body = $result['body'] ?? '';
        $shouldParseGrpcBody = $grpcStatus === \Grpc\STATUS_OK
            || ($grpcStatus === \Grpc\STATUS_UNKNOWN && $details === 'missing grpc-status trailer');
        if (is_string($body) && $shouldParseGrpcBody) {
            $offset = 0;
            $bodyLength = strlen($body);
            while ($offset + 5 <= $bodyLength) {
                if (ord($body[$offset]) !== 0) {
                    $grpcStatus = \Grpc\STATUS_UNIMPLEMENTED;
                    $details = 'compressed gRPC messages are not supported';
                    $payloads = [];
                    break;
                }
                $payloadLength = unpack('N', substr($body, $offset + 1, 4))[1];
                if ($offset + 5 + $payloadLength > $bodyLength) {
                    break;
                }
                $payloads[] = substr($body, $offset + 5, $payloadLength);
                $offset += 5 + $payloadLength;
            }
            if ($grpcStatus === \Grpc\STATUS_OK && $offset !== $bodyLength) {
                $grpcStatus = \Grpc\STATUS_INTERNAL;
                $details = 'malformed gRPC response frame: incomplete trailing bytes';
                $payloads = [];
            } elseif ($grpcStatus === \Grpc\STATUS_OK && count($payloads) !== 1) {
                $grpcStatus = \Grpc\STATUS_INTERNAL;
                $details = count($payloads) === 0
                    ? 'malformed gRPC response frame: missing frame header'
                    : 'malformed unary gRPC response: multiple frames';
                $payloads = [];
            }
        }

        return [
            'payloads' => $payloads,
            'grpc_status' => $grpcStatus,
            'details' => $details,
            'http_status' => (int) ($result['http_status'] ?? 0),
            'headers' => self::extractInitialMetadata($result),
            'trailers' => self::extractTrailers($result, $grpcStatus, $details),
            'raw' => $result,
        ];
    }

    /**
     * @param array<string, list<string>> $headers
     * @return resource
     */
    public static function streamOpen(
        string $target,
        string $path,
        string $serializedRequest,
        array $headers,
        int $timeoutMicros = 0,
        ?\Grpc\ChannelCredentials $credentials = null,
    ): mixed {
        if (!function_exists('nghttp2_poc_stream_open')) {
            throw new \RuntimeException('nghttp2_poc stream API is not loaded');
        }

        [$host, $port] = self::splitTarget($target);
        $useTls = $credentials !== null && !$credentials->isInsecure();

        return \nghttp2_poc_stream_open(
            self::channelKey($host, $port, $credentials),
            $host,
            $port,
            $path,
            $serializedRequest,
            $headers,
            $timeoutMicros,
            $useTls,
            $credentials?->rootCerts,
            $credentials?->certChain,
            $credentials?->privateKey,
        );
    }

    /** @return array{done: bool, payload?: string, raw?: array<string, mixed>, grpc_status?: int, details?: string, http_status?: int, headers?: array<string, list<string>>, trailers?: array<string, list<string>>} */
    public static function streamNext(mixed $stream): array
    {
        if (!function_exists('nghttp2_poc_stream_next')) {
            throw new \RuntimeException('nghttp2_poc stream API is not loaded');
        }

        $result = \nghttp2_poc_stream_next($stream);
        if (($result['done'] ?? false) !== true) {
            return [
                'done' => false,
                'payload' => is_string($result['payload'] ?? null) ? $result['payload'] : '',
            ];
        }

        [$grpcStatus, $details] = self::normalizeStatus($result);
        return [
            'done' => true,
            'grpc_status' => $grpcStatus,
            'details' => $details,
            'http_status' => (int) ($result['http_status'] ?? 0),
            'headers' => self::extractInitialMetadata($result),
            'trailers' => self::extractTrailers($result, $grpcStatus, $details),
            'raw' => $result,
        ];
    }

    public static function streamCancel(mixed $stream): void
    {
        if (function_exists('nghttp2_poc_stream_cancel')) {
            \nghttp2_poc_stream_cancel($stream);
        }
    }

    /** @param array<string, mixed> $result */
    private static function normalizeStatus(array $result): array
    {
        if (($result['timed_out'] ?? false) === true) {
            return [\Grpc\STATUS_DEADLINE_EXCEEDED, 'native transport deadline exceeded'];
        }

        $metadata = self::extractInitialMetadata($result);
        $unsupportedEncoding = self::unsupportedGrpcEncoding($metadata);
        if ($unsupportedEncoding !== null) {
            return [\Grpc\STATUS_UNIMPLEMENTED, "unsupported grpc-encoding: $unsupportedEncoding"];
        }
        if (($result['compressed_response_seen'] ?? false) === true) {
            return [\Grpc\STATUS_UNIMPLEMENTED, 'compressed gRPC messages are not supported'];
        }

        $grpcStatus = (int) ($result['grpc_status'] ?? -1);
        if ($grpcStatus >= 0) {
            $message = $result['grpc_message'] ?? '';
            return [$grpcStatus, is_string($message) ? rawurldecode($message) : ''];
        }

        $streamErrorCode = (int) ($result['stream_error_code'] ?? 0);
        if ($streamErrorCode !== 0) {
            return [self::mapHttp2ErrorToGrpcStatus($streamErrorCode), "HTTP/2 stream reset: $streamErrorCode"];
        }

        $httpStatus = (int) ($result['http_status'] ?? 0);
        if ($httpStatus !== 200) {
            return [self::mapHttpStatusToGrpcStatus($httpStatus), "HTTP status $httpStatus without grpc-status"];
        }

        $contentType = strtolower($metadata['content-type'][0] ?? '');
        if (!str_starts_with($contentType, 'application/grpc')) {
            return [\Grpc\STATUS_UNKNOWN, "invalid gRPC content-type: " . ($contentType === '' ? '<missing>' : $contentType)];
        }

        return [\Grpc\STATUS_UNKNOWN, 'missing grpc-status trailer'];
    }

    private static function mapHttpStatusToGrpcStatus(int $httpStatus): int
    {
        return match ($httpStatus) {
            400 => \Grpc\STATUS_INTERNAL,
            401 => \Grpc\STATUS_UNAUTHENTICATED,
            403 => \Grpc\STATUS_PERMISSION_DENIED,
            404 => \Grpc\STATUS_UNIMPLEMENTED,
            429, 502, 503, 504 => \Grpc\STATUS_UNAVAILABLE,
            default => \Grpc\STATUS_UNKNOWN,
        };
    }

    private static function mapHttp2ErrorToGrpcStatus(int $streamErrorCode): int
    {
        return match ($streamErrorCode) {
            0x8 => \Grpc\STATUS_CANCELLED,
            0x2 => \Grpc\STATUS_INTERNAL,
            0x7 => \Grpc\STATUS_UNAVAILABLE,
            default => \Grpc\STATUS_UNAVAILABLE,
        };
    }

    /**
     * @param array<string, mixed> $result
     * @return array<string, list<string>>
     */
    private static function extractInitialMetadata(array $result): array
    {
        return self::normalizeMetadataMap($result['initial_metadata'] ?? []);
    }

    /**
     * @param array<string, list<string>> $metadata
     */
    private static function unsupportedGrpcEncoding(array $metadata): ?string
    {
        $encoding = strtolower($metadata['grpc-encoding'][0] ?? 'identity');
        return ($encoding === '' || $encoding === 'identity') ? null : $encoding;
    }

    /**
     * @param mixed $metadata
     * @return array<string, list<string>>
     */
    private static function normalizeMetadataMap(mixed $metadata): array
    {
        if (!is_array($metadata)) {
            return [];
        }

        $normalized = [];
        foreach ($metadata as $key => $values) {
            if (!is_string($key) || str_starts_with($key, ':')) {
                continue;
            }
            foreach ((array) $values as $value) {
                if (!is_string($value)) {
                    continue;
                }
                if (str_ends_with(strtolower($key), '-bin')) {
                    foreach (explode(',', $value) as $part) {
                        $decoded = base64_decode($part, true);
                        $normalized[$key][] = $decoded === false ? '' : $decoded;
                    }
                    continue;
                }
                $normalized[$key][] = $value;
            }
        }

        return $normalized;
    }

    /**
     * @param array<string, mixed> $result
     * @return array<string, list<string>>
     */
    private static function extractTrailers(array $result, int $grpcStatus, string $details): array
    {
        $trailers = self::normalizeMetadataMap($result['trailing_metadata'] ?? []);
        $trailers['grpc-status'] = [(string) $grpcStatus];
        if ($details !== '') {
            $trailers['grpc-message'] = [rawurlencode($details)];
        }

        $serverStatsFields = [
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
        ];

        $hasServerStats = false;
        foreach ($serverStatsFields as $field) {
            $value = self::firstScalarValue($result[$field] ?? null);
            if ($value !== null && (string) $value !== '0') {
                $hasServerStats = true;
                break;
            }
        }
        if (!$hasServerStats) {
            return $trailers;
        }

        foreach ($serverStatsFields as $field) {
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

    private static function channelKey(string $host, int $port, ?\Grpc\ChannelCredentials $credentials): string
    {
        return implode('|', [
            $host . ':' . $port,
            $credentials?->type ?? \Grpc\ChannelCredentials::TYPE_INSECURE,
            sha1($credentials?->rootCerts ?? ''),
            sha1($credentials?->certChain ?? ''),
            sha1($credentials?->privateKey ?? ''),
        ]);
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
