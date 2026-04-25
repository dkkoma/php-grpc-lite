<?php
declare(strict_types=1);

namespace Grpc;

/**
 * A Channel decorator that carries a list of Interceptors. Constructed via
 * `Interceptor::intercept()` and consumed by `BaseStub` when dispatching
 * calls — BaseStub detects this subclass, builds an interceptor chain, and
 * unwraps to the inner Channel for the actual transport.
 */
final class InterceptorChannel extends Channel
{
    /**
     * @param list<Interceptor> $interceptors outermost first
     */
    public function __construct(
        public readonly Channel $inner,
        public readonly array $interceptors,
    ) {
        parent::__construct($inner->hostname, $inner->opts);
    }

    public function getInnerChannel(): Channel
    {
        return $this->inner;
    }

    /** @return list<Interceptor> */
    public function getInterceptors(): array
    {
        return $this->interceptors;
    }
}
