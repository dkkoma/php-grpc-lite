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
                $headers[] = $key . ': ' . $v;
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
        $ch = curl_init();
        if ($ch === false) {
            throw new \RuntimeException('failed to initialize curl handle');
        }
        return $ch;
    }
}
