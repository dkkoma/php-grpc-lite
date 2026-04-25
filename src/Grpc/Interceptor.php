<?php
declare(strict_types=1);

namespace Grpc;

/**
 * Base class for user-defined interceptors. Subclass and override the
 * relevant intercept* method(s); the default implementations forward
 * unchanged through to the supplied continuation.
 *
 * Apply interceptors to a Channel via the static `intercept()` factory:
 *
 *   $channel = Interceptor::intercept($channel, [$interceptorA, $interceptorB]);
 *   $stub    = new MyServiceStub($host, $opts, $channel);
 */
abstract class Interceptor
{
    /**
     * Wrap a Channel so that calls dispatched through stubs using it pass
     * through the given interceptor list (outermost first).
     *
     * @param Interceptor|list<Interceptor>|null $interceptors
     */
    public static function intercept(Channel $channel, $interceptors): Channel
    {
        if ($interceptors === null) {
            return $channel;
        }
        if (!is_array($interceptors)) {
            $interceptors = [$interceptors];
        }
        if ($interceptors === []) {
            return $channel;
        }
        return new InterceptorChannel($channel, array_values($interceptors));
    }

    /**
     * @param callable(string, object, mixed, array, array): UnaryCall $continuation
     * @param array<string, string|list<string>> $metadata
     * @param array<string, mixed> $options
     */
    public function interceptUnaryUnary(
        string $method,
        object $argument,
        $deserialize,
        callable $continuation,
        array $metadata = [],
        array $options = [],
    ): UnaryCall {
        return $continuation($method, $argument, $deserialize, $metadata, $options);
    }

    /**
     * @param callable(string, object, mixed, array, array): ServerStreamingCall $continuation
     * @param array<string, string|list<string>> $metadata
     * @param array<string, mixed> $options
     */
    public function interceptUnaryStream(
        string $method,
        object $argument,
        $deserialize,
        callable $continuation,
        array $metadata = [],
        array $options = [],
    ): ServerStreamingCall {
        return $continuation($method, $argument, $deserialize, $metadata, $options);
    }

    /**
     * @param callable(string, mixed, array, array): ClientStreamingCall $continuation
     * @param array<string, string|list<string>> $metadata
     * @param array<string, mixed> $options
     */
    public function interceptStreamUnary(
        string $method,
        $deserialize,
        callable $continuation,
        array $metadata = [],
        array $options = [],
    ): ClientStreamingCall {
        return $continuation($method, $deserialize, $metadata, $options);
    }

    /**
     * @param callable(string, mixed, array, array): BidiStreamingCall $continuation
     * @param array<string, string|list<string>> $metadata
     * @param array<string, mixed> $options
     */
    public function interceptStreamStream(
        string $method,
        $deserialize,
        callable $continuation,
        array $metadata = [],
        array $options = [],
    ): BidiStreamingCall {
        return $continuation($method, $deserialize, $metadata, $options);
    }
}
