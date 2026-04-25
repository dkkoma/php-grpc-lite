<?php
declare(strict_types=1);

namespace Grpc;

/**
 * Stream of requests → single response. **Not implemented in Phase 0.** The
 * class exists so that generated stubs that reference it can autoload, and
 * to reserve the API surface; any actual call dispatch raises a clear
 * `BadMethodCallException`.
 */
class ClientStreamingCall extends AbstractCall
{
    public function start(array $metadata = [], array $options = []): void
    {
        throw new \BadMethodCallException(
            'ClientStreamingCall is not yet implemented in php-grpc-lite Phase 0'
        );
    }

    public function write(object $request, array $options = []): void
    {
        throw new \BadMethodCallException('ClientStreamingCall::write is not yet implemented');
    }

    public function wait(): array
    {
        throw new \BadMethodCallException('ClientStreamingCall::wait is not yet implemented');
    }

    public function cancel(): void
    {
        // no-op
    }

    /** @return array<string, list<string>> */
    public function getMetadata(): array
    {
        return [];
    }

    /** @return array<string, list<string>> */
    public function getTrailingMetadata(): array
    {
        return [];
    }
}
