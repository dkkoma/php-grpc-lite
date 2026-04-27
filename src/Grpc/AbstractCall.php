<?php
declare(strict_types=1);

namespace Grpc;

/**
 * Common base for all gRPC call objects. Holds the libcurl handle, channel
 * reference, request/response metadata, and helpers for building the HTTP
 * request from gRPC-level inputs.
 */
abstract class AbstractCall
{
    protected ?\CurlHandle $ch = null;

    /** @var callable|array{class-string, string}|null */
    protected $deserialize;

    /** @var array<string, string|list<string>> */
    protected array $metadata;

    /** @var array<string, mixed> */
    protected array $options;

    /**
     * @param callable|array{class-string, string}|null $deserialize see Internal\Deserialize::apply
     */
    public function __construct(
        protected readonly Channel $channel,
        protected readonly string $method,
        $deserialize,
        array $metadata = [],
        array $options = [],
    ) {
        $this->deserialize = $deserialize;
        $this->metadata = $metadata;
        $this->options = $options;
    }

    public function getPeer(): string
    {
        return $this->channel->getTarget();
    }

    abstract public function cancel(): void;

    /** @return array<string, list<string>> */
    abstract public function getMetadata(): array;

    /** @return array<string, list<string>> */
    abstract public function getTrailingMetadata(): array;

    protected function buildUrl(): string
    {
        $scheme = $this->channel->credentials->isInsecure() ? 'http' : 'https';
        return $scheme . '://' . $this->channel->hostname . $this->method;
    }

    protected function getHttpVersion(): int
    {
        return $this->channel->credentials->isInsecure()
            ? CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE
            : CURL_HTTP_VERSION_2TLS;
    }

    /**
     * Compose the HTTP header list for the request: required gRPC headers
     * + grpc-timeout + caller metadata + per-call credentials.
     *
     * @return list<string>
     */
    protected function buildRequestHeaders(): array
    {
        $metadata = $this->metadata;

        $updateMetadata = $this->channel->opts['update_metadata'] ?? null;
        if (is_callable($updateMetadata)) {
            $metadata = $updateMetadata($metadata);
        }

        $callCredCb = $this->options['call_credentials_callback'] ?? null;
        if (is_callable($callCredCb)) {
            $extra = $callCredCb($this->buildUrl(), $this->method);
            foreach ((array) $extra as $k => $v) {
                $metadata[$k] = is_array($v) ? $v : [$v];
            }
        }

        $headers = [
            'Content-Type: application/grpc',
            'TE: trailers',
            'User-Agent: ' . ($this->channel->opts['grpc.primary_user_agent'] ?? 'php-grpc-lite/0.0.1'),
        ];

        if (isset($this->options['timeout'])) {
            $headers[] = 'grpc-timeout: ' . (int) $this->options['timeout'] . 'u';
        }

        foreach ($metadata as $key => $values) {
            foreach ((array) $values as $v) {
                $value = $this->isBinaryMetadataKey($key)
                    ? base64_encode((string) $v)
                    : (string) $v;
                $headers[] = $key . ': ' . $value;
            }
        }

        return $headers;
    }

    /** @param array<string, string|list<string>> $metadata */
    protected function mergeStartArgs(array $metadata, array $options): void
    {
        if ($metadata !== []) {
            $this->metadata = $metadata + $this->metadata;
        }
        if ($options !== []) {
            $this->options = $options + $this->options;
        }
    }

    protected function initCurl(): \CurlHandle
    {
        return $this->channel->acquireCurlHandle();
    }

    protected function releaseCurl(\CurlHandle $ch): void
    {
        $this->channel->releaseCurlHandle($ch);
    }

    protected function discardCurl(\CurlHandle $ch): void
    {
        $this->channel->discardCurlHandle($ch);
    }

    /**
     * Apply TLS-specific curl options based on the channel's credentials.
     * No-op for insecure channels.
     */
    protected function applyTlsOptions(\CurlHandle $ch): void
    {
        $creds = $this->channel->credentials;
        if ($creds->isInsecure()) {
            return;
        }

        if ($creds->rootCerts !== null) {
            if (defined('CURLOPT_CAINFO_BLOB')) {
                curl_setopt($ch, \CURLOPT_CAINFO_BLOB, $creds->rootCerts);
            } else {
                curl_setopt($ch, CURLOPT_CAINFO, self::writeTempPem($creds->rootCerts, 'ca'));
            }
        }

        if ($creds->certChain !== null && $creds->privateKey !== null) {
            if (defined('CURLOPT_SSLCERT_BLOB')) {
                curl_setopt($ch, \CURLOPT_SSLCERT_BLOB, $creds->certChain);
                curl_setopt($ch, \CURLOPT_SSLKEY_BLOB, $creds->privateKey);
            } else {
                curl_setopt($ch, CURLOPT_SSLCERT, self::writeTempPem($creds->certChain, 'cert'));
                curl_setopt($ch, CURLOPT_SSLKEY, self::writeTempPem($creds->privateKey, 'key'));
            }
        }
    }

    protected function applyTimeoutOptions(\CurlHandle $ch): void
    {
        if (!isset($this->options['timeout'])) {
            return;
        }

        $timeoutMicros = max(1, (int) $this->options['timeout']);
        $timeoutMillis = max(1, (int) ceil($timeoutMicros / 1000));
        curl_setopt($ch, CURLOPT_TIMEOUT_MS, $timeoutMillis);
        curl_setopt($ch, CURLOPT_CONNECTTIMEOUT_MS, $timeoutMillis);
    }

    protected function decodeGrpcMessage(string $message): string
    {
        return rawurldecode($message);
    }

    protected function mapHttpStatusToGrpcStatus(int $httpStatus): int
    {
        return match ($httpStatus) {
            400 => STATUS_INTERNAL,
            401 => STATUS_UNAUTHENTICATED,
            403 => STATUS_PERMISSION_DENIED,
            404 => STATUS_UNIMPLEMENTED,
            429, 502, 503, 504 => STATUS_UNAVAILABLE,
            default => STATUS_UNKNOWN,
        };
    }

    /** @param array<string, list<string>> $headers */
    protected function findUnsupportedGrpcEncoding(array $headers): ?string
    {
        $encoding = strtolower($headers['grpc-encoding'][0] ?? 'identity');
        return ($encoding === '' || $encoding === 'identity') ? null : $encoding;
    }

    protected function isBinaryMetadataKey(string $key): bool
    {
        return str_ends_with(strtolower($key), '-bin');
    }

    /** @param array<string, list<string>> $metadata */
    protected function appendMetadataHeader(array &$metadata, string $key, string $value): void
    {
        foreach ($this->decodeMetadataHeader($key, $value) as $decoded) {
            $metadata[$key][] = $decoded;
        }
    }

    /** @return list<string> */
    protected function decodeMetadataHeader(string $key, string $value): array
    {
        if (!$this->isBinaryMetadataKey($key)) {
            return [$value];
        }

        $decoded = [];
        foreach (explode(',', $value) as $part) {
            $binary = base64_decode($part, true);
            $decoded[] = $binary === false ? '' : $binary;
        }
        return $decoded;
    }

    /**
     * Cache PEM material to a tmpfile keyed by content hash so that identical
     * inputs reuse the same file across calls.
     */
    private static function writeTempPem(string $pem, string $kind): string
    {
        $path = sys_get_temp_dir()
            . '/php-grpc-lite-' . $kind . '-'
            . substr(sha1($pem), 0, 16) . '.pem';
        if (!file_exists($path)) {
            file_put_contents($path, $pem);
            @chmod($path, 0600);
        }
        return $path;
    }
}
