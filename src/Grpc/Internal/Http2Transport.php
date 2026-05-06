<?php
declare(strict_types=1);

namespace Grpc\Internal;

/**
 * Thin PHP wrapper around the HTTP/2 transport extension.
 *
 * Production unary and server-streaming calls use C-owned persistent channels;
 * benchmark-only extension entrypoints are called directly from bench scripts.
 */
final class Http2Transport
{
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
        int $maxReceiveMessageLength = 0,
        ?string $authority = null,
        ?string $tlsVerifyName = null,
    ): array {
        if (!function_exists('grpc_lite_unary')) {
            throw new \RuntimeException('grpc lite extension bridge is not loaded');
        }
        if (strlen($serializedRequest) > 0xffffffff) {
            throw new \RuntimeException('gRPC request message exceeds 32-bit frame length');
        }

        [$host, $port] = self::splitTarget($target);
        $framedRequest = "\0" . pack('N', strlen($serializedRequest)) . $serializedRequest;
        $key = self::channelKey($host, $port, $credentials, $authority, $tlsVerifyName);
        try {
            $useTls = $credentials !== null && !$credentials->isInsecure();
            $result = \grpc_lite_unary(
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
                $maxReceiveMessageLength,
                $authority,
                $tlsVerifyName,
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
        int $maxReceiveMessageLength = 0,
        ?string $authority = null,
        ?string $tlsVerifyName = null,
    ): mixed {
        if (!function_exists('grpc_lite_stream_open')) {
            throw new \RuntimeException('grpc lite extension bridge is not loaded');
        }

        [$host, $port] = self::splitTarget($target);
        $useTls = $credentials !== null && !$credentials->isInsecure();

        return \grpc_lite_stream_open(
            self::channelKey($host, $port, $credentials, $authority, $tlsVerifyName),
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
            $maxReceiveMessageLength,
            $authority,
            $tlsVerifyName,
        );
    }

    /** @return array{done: bool, payload?: string, raw?: array<string, mixed>, grpc_status?: int, details?: string, http_status?: int, headers?: array<string, list<string>>, trailers?: array<string, list<string>>} */
    public static function streamNext(mixed $stream): array
    {
        if (!function_exists('grpc_lite_stream_next')) {
            throw new \RuntimeException('grpc lite extension bridge is not loaded');
        }

        $result = \grpc_lite_stream_next($stream);
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
        if (function_exists('grpc_lite_stream_cancel')) {
            \grpc_lite_stream_cancel($stream);
        }
    }

    public static function closeChannel(
        string $target,
        ?\Grpc\ChannelCredentials $credentials = null,
        ?string $authority = null,
        ?string $tlsVerifyName = null,
    ): void
    {
        if (!function_exists('grpc_lite_channel_close')) {
            return;
        }

        [$host, $port] = self::splitTarget($target);
        \grpc_lite_channel_close(self::channelKey($host, $port, $credentials, $authority, $tlsVerifyName));
    }

    /** @param array<string, mixed> $opts */
    public static function authorityOverride(array $opts): ?string
    {
        $authority = $opts['grpc.default_authority']
            ?? $opts['grpc.ssl_target_name_override']
            ?? null;
        if ($authority === null) {
            return null;
        }
        if (!is_string($authority)) {
            throw new \InvalidArgumentException('grpc.default_authority must be a string');
        }
        if ($authority === '' || str_contains($authority, "\r") || str_contains($authority, "\n")) {
            throw new \InvalidArgumentException('invalid grpc.default_authority');
        }

        return $authority;
    }

    /** @param array<string, mixed> $opts */
    public static function tlsVerifyNameOverride(array $opts): ?string
    {
        $name = $opts['grpc.ssl_target_name_override'] ?? null;
        if ($name === null) {
            return null;
        }
        if (!is_string($name)) {
            throw new \InvalidArgumentException('grpc.ssl_target_name_override must be a string');
        }
        if ($name === '' || str_contains($name, "\r") || str_contains($name, "\n")) {
            throw new \InvalidArgumentException('invalid grpc.ssl_target_name_override');
        }

        return $name;
    }

    /** @param array<string, mixed> $result */
    private static function normalizeStatus(array $result): array
    {
        if (($result['timed_out'] ?? false) === true) {
            return [\Grpc\STATUS_DEADLINE_EXCEEDED, 'HTTP/2 transport deadline exceeded'];
        }

        $metadata = self::extractInitialMetadata($result);
        $trailingMetadata = self::normalizeMetadataMap($result['trailing_metadata'] ?? []);
        $httpStatus = (int) ($result['http_status'] ?? 0);
        $grpcStatus = (int) ($result['grpc_status'] ?? -1);
        if ($httpStatus !== 0 && $httpStatus !== 200 && $grpcStatus < 0) {
            return [self::mapHttpStatusToGrpcStatus($httpStatus), "HTTP status $httpStatus without grpc-status"];
        }

        $contentType = strtolower($metadata['content-type'][0] ?? $trailingMetadata['content-type'][0] ?? '');
        if ($contentType !== '' && !str_starts_with($contentType, 'application/grpc')) {
            return [\Grpc\STATUS_UNKNOWN, "invalid gRPC content-type: $contentType"];
        }

        if (($result['response_message_too_large'] ?? false) === true) {
            return [\Grpc\STATUS_RESOURCE_EXHAUSTED, 'received message exceeds maximum size'];
        }
        if (($result['metadata_too_large'] ?? false) === true) {
            return [\Grpc\STATUS_RESOURCE_EXHAUSTED, 'received metadata exceeds maximum size'];
        }
        if (($result['malformed_response_frame'] ?? false) === true) {
            return [\Grpc\STATUS_INTERNAL, 'malformed gRPC response frame: incomplete trailing bytes'];
        }

        $unsupportedEncoding = self::unsupportedGrpcEncoding($metadata);
        if ($unsupportedEncoding !== null) {
            return [\Grpc\STATUS_UNIMPLEMENTED, "unsupported grpc-encoding: $unsupportedEncoding"];
        }
        if (($result['compressed_response_seen'] ?? false) === true) {
            return [\Grpc\STATUS_UNIMPLEMENTED, 'compressed gRPC messages are not supported'];
        }

        $streamErrorCode = (int) ($result['stream_error_code'] ?? 0);
        if ($streamErrorCode !== 0) {
            return [self::mapHttp2ErrorToGrpcStatus($streamErrorCode), "HTTP/2 stream reset: $streamErrorCode"];
        }

        if (($result['invalid_grpc_status'] ?? false) === true) {
            return [\Grpc\STATUS_UNKNOWN, 'invalid grpc-status trailer'];
        }

        if ($grpcStatus >= 0) {
            $message = $result['grpc_message'] ?? '';
            return [$grpcStatus, is_string($message) ? rawurldecode($message) : ''];
        }

        if (($result['channel_dead'] ?? false) === true) {
            $detail = $result['channel_last_error_detail'] ?? '';
            return [\Grpc\STATUS_UNAVAILABLE, is_string($detail) && $detail !== '' ? $detail : 'HTTP/2 transport I/O error'];
        }

        if ($contentType === '') {
            return [\Grpc\STATUS_UNKNOWN, "invalid gRPC content-type: " . ($contentType === '' ? '<missing>' : $contentType)];
        }

        if ($httpStatus !== 200) {
            return [self::mapHttpStatusToGrpcStatus($httpStatus), "HTTP status $httpStatus without grpc-status"];
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
            0xb => \Grpc\STATUS_RESOURCE_EXHAUSTED,
            0xc => \Grpc\STATUS_PERMISSION_DENIED,
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

        return $trailers;
    }

    private static function channelKey(string $host, int $port, ?\Grpc\ChannelCredentials $credentials, ?string $authority, ?string $tlsVerifyName): string
    {
        return implode('|', [
            $host . ':' . $port,
            $authority ?? '',
            $tlsVerifyName ?? '',
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
