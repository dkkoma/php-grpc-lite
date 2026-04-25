<?php
declare(strict_types=1);

namespace Grpc;

/**
 * Stream of requests → stream of responses. **Not implemented in Phase 0.**
 * See ClientStreamingCall for rationale.
 */
class BidiStreamingCall extends AbstractCall
{
    public function start(array $metadata = [], array $options = []): void
    {
        throw new \BadMethodCallException(
            'BidiStreamingCall is not yet implemented in php-grpc-lite Phase 0'
        );
    }

    public function write(object $request, array $options = []): void
    {
        throw new \BadMethodCallException('BidiStreamingCall::write is not yet implemented');
    }

    public function read(): ?object
    {
        throw new \BadMethodCallException('BidiStreamingCall::read is not yet implemented');
    }

    public function writesDone(): void
    {
        throw new \BadMethodCallException('BidiStreamingCall::writesDone is not yet implemented');
    }

    public function getStatus(): \stdClass
    {
        throw new \BadMethodCallException('BidiStreamingCall::getStatus is not yet implemented');
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
