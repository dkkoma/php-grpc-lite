<?php
declare(strict_types=1);

namespace Grpc;

use Grpc\Internal\Http2Transport;

/**
 * Holds the address and channel-level options for a gRPC peer.
 */
class Channel
{
    public readonly string $hostname;

    /** @var array<string, mixed> */
    public readonly array $opts;

    public readonly ChannelCredentials $credentials;

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

    public function close(): void
    {
        Http2Transport::closeChannel(
            $this->hostname,
            $this->credentials,
            Http2Transport::authorityOverride($this->opts),
        );
    }
}
