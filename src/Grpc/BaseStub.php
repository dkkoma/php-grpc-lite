<?php
declare(strict_types=1);

namespace Grpc;

/**
 * Base class for generated service stubs. Generated `*GrpcClient` classes
 * extend this and call the protected `_simpleRequest` / `_serverStreamRequest`
 * helpers to dispatch RPCs.
 */
abstract class BaseStub
{
    protected Channel $channel;
    protected readonly string $hostname;

    /** @var array<string, mixed> */
    protected readonly array $opts;

    /**
     * @param array<string, mixed> $opts must contain a 'credentials' key
     *        with a ChannelCredentials instance unless $channel is supplied.
     */
    public function __construct(string $hostname, array $opts, ?Channel $channel = null)
    {
        $this->hostname = $hostname;
        $this->opts = $opts;
        $this->channel = $channel ?? new Channel($hostname, $opts);
    }

    /**
     * Dispatch a unary RPC.
     *
     * @param string $method e.g. '/helloworld.Greeter/SayHello'
     * @param object $argument message instance with serializeToString()
     * @param callable|array{class-string, string} $deserialize either a real
     *        callable, or the gax-style `[ClassName::class, 'decode']` form
     *        (where `decode` does not actually exist as a method — see
     *        Internal\Deserialize::apply).
     * @param array<string, string|list<string>> $metadata
     * @param array<string, mixed> $options
     */
    protected function _simpleRequest(
        string $method,
        object $argument,
        $deserialize,
        array $metadata = [],
        array $options = [],
    ): UnaryCall {
        $call = new UnaryCall($this->channel, $method, $deserialize, $metadata, $options);
        $call->start($argument);
        return $call;
    }

    public function close(): void
    {
        $this->channel->close();
    }

    public function getTarget(): string
    {
        return $this->hostname;
    }
}
