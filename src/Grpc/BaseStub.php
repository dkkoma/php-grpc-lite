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
        $continuation = function (string $m, object $a, $d, array $md, array $o): UnaryCall {
            $call = new UnaryCall($this->getInnerChannel(), $m, $d, $md, $o);
            $call->start($a);
            return $call;
        };
        if ($this->channel instanceof InterceptorChannel) {
            $chained = $this->buildUnaryChain($this->channel->getInterceptors(), $continuation);
            return $chained($method, $argument, $deserialize, $metadata, $options);
        }
        return $continuation($method, $argument, $deserialize, $metadata, $options);
    }

    /**
     * Dispatch a server-streaming RPC.
     *
     * @param string $method e.g. '/helloworld.Greeter/SayManyHellos'
     * @param object $argument message instance with serializeToString()
     * @param callable|array{class-string, string} $deserialize see _simpleRequest
     * @param array<string, string|list<string>> $metadata
     * @param array<string, mixed> $options
     */
    protected function _serverStreamRequest(
        string $method,
        object $argument,
        $deserialize,
        array $metadata = [],
        array $options = [],
    ): ServerStreamingCall {
        $continuation = function (string $m, object $a, $d, array $md, array $o): ServerStreamingCall {
            $call = new ServerStreamingCall($this->getInnerChannel(), $m, $d, $md, $o);
            $call->start($a);
            return $call;
        };
        if ($this->channel instanceof InterceptorChannel) {
            $chained = $this->buildServerStreamChain($this->channel->getInterceptors(), $continuation);
            return $chained($method, $argument, $deserialize, $metadata, $options);
        }
        return $continuation($method, $argument, $deserialize, $metadata, $options);
    }

    /**
     * Stub for Phase 0 — client streaming is not yet implemented.
     *
     * @param array<string, string|list<string>> $metadata
     * @param array<string, mixed> $options
     */
    protected function _clientStreamRequest(
        string $method,
        $deserialize,
        array $metadata = [],
        array $options = [],
    ): ClientStreamingCall {
        throw new \BadMethodCallException(
            'client streaming is not yet implemented in php-grpc-lite Phase 0'
        );
    }

    /**
     * Stub for Phase 0 — bidirectional streaming is not yet implemented.
     *
     * @param array<string, string|list<string>> $metadata
     * @param array<string, mixed> $options
     */
    protected function _bidiRequest(
        string $method,
        $deserialize,
        array $metadata = [],
        array $options = [],
    ): BidiStreamingCall {
        throw new \BadMethodCallException(
            'bidirectional streaming is not yet implemented in php-grpc-lite Phase 0'
        );
    }

    private function getInnerChannel(): Channel
    {
        return $this->channel instanceof InterceptorChannel
            ? $this->channel->getInnerChannel()
            : $this->channel;
    }

    /**
     * @param list<Interceptor> $interceptors outermost first
     * @param callable(string, object, mixed, array, array): UnaryCall $innermost
     * @return callable(string, object, mixed, array, array): UnaryCall
     */
    private function buildUnaryChain(array $interceptors, callable $innermost): callable
    {
        $next = $innermost;
        foreach (array_reverse($interceptors) as $interceptor) {
            $captured = $next;
            $next = static function (
                string $m, object $a, $d, array $md, array $o,
            ) use ($interceptor, $captured): UnaryCall {
                return $interceptor->interceptUnaryUnary($m, $a, $d, $captured, $md, $o);
            };
        }
        return $next;
    }

    /**
     * @param list<Interceptor> $interceptors
     * @param callable(string, object, mixed, array, array): ServerStreamingCall $innermost
     * @return callable(string, object, mixed, array, array): ServerStreamingCall
     */
    private function buildServerStreamChain(array $interceptors, callable $innermost): callable
    {
        $next = $innermost;
        foreach (array_reverse($interceptors) as $interceptor) {
            $captured = $next;
            $next = static function (
                string $m, object $a, $d, array $md, array $o,
            ) use ($interceptor, $captured): ServerStreamingCall {
                return $interceptor->interceptUnaryStream($m, $a, $d, $captured, $md, $o);
            };
        }
        return $next;
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
