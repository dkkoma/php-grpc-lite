<?php
declare(strict_types=1);

namespace Grpc;

/**
 * Holds the address, channel-level options, and reusable libcurl easy handles
 * for a gRPC peer.
 */
class Channel
{
    public readonly string $hostname;

    /** @var array<string, mixed> */
    public readonly array $opts;

    public readonly ChannelCredentials $credentials;

    /** @var list<\CurlHandle> */
    private array $idleCurlHandles = [];

    private bool $closed = false;

    /**
     * @param array<string, mixed> $opts Channel options. The 'credentials' key
     *        is required and must hold a ChannelCredentials instance.
     */
    public function __construct(string $hostname, array $opts)
    {
        $credentials = $opts['credentials'] ?? null;
        if (!($credentials instanceof ChannelCredentials)) {
            throw new \InvalidArgumentException(
                "Channel options must include a 'credentials' key with a "
                . ChannelCredentials::class . ' instance'
            );
        }
        $this->hostname = $hostname;
        $this->opts = $opts;
        $this->credentials = $credentials;
    }

    public function getTarget(): string
    {
        return $this->hostname;
    }

    /**
     * @internal
     */
    public function acquireCurlHandle(): \CurlHandle
    {
        $this->closed = false;
        return array_pop($this->idleCurlHandles) ?? $this->createCurlHandle();
    }

    /**
     * @internal
     */
    public function releaseCurlHandle(\CurlHandle $ch): void
    {
        curl_reset($ch);
        if ($this->closed) {
            curl_close($ch);
            return;
        }
        $this->idleCurlHandles[] = $ch;
    }

    /**
     * @internal
     */
    public function discardCurlHandle(\CurlHandle $ch): void
    {
        curl_close($ch);
    }

    public function close(): void
    {
        $this->closed = true;
        foreach ($this->idleCurlHandles as $ch) {
            curl_close($ch);
        }
        $this->idleCurlHandles = [];
    }

    private function createCurlHandle(): \CurlHandle
    {
        $ch = curl_init();
        if ($ch === false) {
            throw new \RuntimeException('failed to initialize curl handle');
        }
        return $ch;
    }
}
