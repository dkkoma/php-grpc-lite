<?php
declare(strict_types=1);

namespace Grpc;

/**
 * Common base for all gRPC call objects. Holds the channel reference,
 * request metadata, and helpers for building native transport inputs.
 */
abstract class AbstractCall
{
    private const METADATA_KEY_PATTERN = '/^[0-9a-z_.-]+$/';

    private const LIBRARY_OWNED_METADATA = [
        'content-type' => true,
        'grpc-accept-encoding' => true,
        'grpc-encoding' => true,
        'grpc-message' => true,
        'grpc-status' => true,
        'grpc-status-details-bin' => true,
        'grpc-timeout' => true,
        'te' => true,
        'user-agent' => true,
    ];

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

    /** @return array<string, list<string>> */
    protected function buildNativeRequestHeaders(): array
    {
        $headers = [
            'user-agent' => [$this->buildUserAgent()],
        ];
        if (isset($this->options['timeout'])) {
            $headers['grpc-timeout'] = [$this->encodeGrpcTimeout((int) $this->options['timeout'])];
        }

        foreach ($this->buildRequestMetadata() as $key => $values) {
            foreach ($values as $value) {
                $headers[$key][] = $this->isBinaryMetadataKey($key)
                    ? base64_encode($value)
                    : $value;
            }
        }

        return $headers;
    }

    protected function buildUserAgent(): string
    {
        $default = 'php-grpc-lite/' . (defined(__NAMESPACE__ . '\\VERSION') ? constant(__NAMESPACE__ . '\\VERSION') : '0.1.0-dev');
        $primary = trim((string) ($this->channel->opts['grpc.primary_user_agent'] ?? ''));
        return $primary === '' ? $default : $primary . ' ' . $default;
    }

    /** @return array<string, list<string>> */
    protected function buildRequestMetadata(): array
    {
        $metadata = $this->metadata;

        $updateMetadata = $this->channel->opts['update_metadata'] ?? null;
        if (is_callable($updateMetadata)) {
            $updated = $updateMetadata($metadata);
            if (!is_array($updated)) {
                throw new \InvalidArgumentException('update_metadata must return an array');
            }
            $metadata = $updated;
        }

        $callCredCb = $this->options['call_credentials_callback'] ?? null;
        if (is_callable($callCredCb)) {
            $extra = $callCredCb($this->buildUrl(), $this->method);
            foreach ((array) $extra as $k => $v) {
                $metadata[$k] = is_array($v) ? $v : [$v];
            }
        }
        $callCredentials = $this->options['call_credentials'] ?? null;
        if ($callCredentials instanceof CallCredentials) {
            foreach ($callCredentials->getMetadata($this->buildUrl(), $this->method) as $k => $v) {
                $metadata[$k] = is_array($v) ? $v : [$v];
            }
        }

        return $this->normalizeRequestMetadata($metadata);
    }

    protected function encodeGrpcTimeout(int $timeoutMicros): string
    {
        $timeoutMicros = max(1, $timeoutMicros);
        foreach ([
            'u' => 1,
            'm' => 1_000,
            'S' => 1_000_000,
            'M' => 60_000_000,
            'H' => 3_600_000_000,
        ] as $unit => $microsPerUnit) {
            $value = (int) ceil($timeoutMicros / $microsPerUnit);
            if ($value <= 99_999_999) {
                return $value . $unit;
            }
        }

        return '99999999H';
    }

    /**
     * @param array<array-key, mixed> $metadata
     * @return array<string, list<string>>
     */
    private function normalizeRequestMetadata(array $metadata): array
    {
        $normalized = [];
        foreach ($metadata as $key => $values) {
            if (!is_string($key)) {
                throw new \InvalidArgumentException('metadata key must be a string');
            }

            $normalizedKey = strtolower($key);
            $this->assertValidMetadataKey($normalizedKey, $key);
            if (isset(self::LIBRARY_OWNED_METADATA[$normalizedKey])) {
                continue;
            }

            foreach ((array) $values as $value) {
                if (!is_scalar($value) && $value !== null) {
                    throw new \InvalidArgumentException("metadata value for '$key' must be scalar");
                }
                $stringValue = (string) $value;
                $this->assertValidMetadataValue($normalizedKey, $stringValue);
                $normalized[$normalizedKey][] = $stringValue;
            }
        }

        return $normalized;
    }

    private function assertValidMetadataKey(string $normalizedKey, string $originalKey): void
    {
        if ($normalizedKey === '' || str_starts_with($normalizedKey, ':')) {
            throw new \InvalidArgumentException("invalid metadata key: $originalKey");
        }
        if (preg_match('/[^\x00-\x7f]/', $originalKey) === 1) {
            throw new \InvalidArgumentException("invalid metadata key: $originalKey");
        }
        if (preg_match(self::METADATA_KEY_PATTERN, $normalizedKey) !== 1) {
            throw new \InvalidArgumentException("invalid metadata key: $originalKey");
        }
    }

    private function assertValidMetadataValue(string $key, string $value): void
    {
        if ($this->isBinaryMetadataKey($key)) {
            return;
        }
        if (str_contains($value, "\r") || str_contains($value, "\n")) {
            throw new \InvalidArgumentException("invalid metadata value for '$key': CR/LF is not allowed");
        }
        if (preg_match('/[^\x00-\x7f]/', $value) === 1) {
            throw new \InvalidArgumentException("invalid metadata value for '$key': non-ASCII is not allowed");
        }
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

    protected function decodeGrpcMessage(string $message): string
    {
        return rawurldecode($message);
    }

    protected function isBinaryMetadataKey(string $key): bool
    {
        return str_ends_with(strtolower($key), '-bin');
    }
}
