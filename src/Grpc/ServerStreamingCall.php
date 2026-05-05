<?php
declare(strict_types=1);

namespace Grpc;

/**
 * One request → stream of responses. Constructed via
 * `BaseStub::_serverStreamRequest`, which calls `start($argument)` to stage the
 * native stream. The caller then iterates `responses()`.
 */
class ServerStreamingCall extends AbstractCall
{
    /** @var array<string, list<string>> */
    private array $responseHeaders = [];

    /** @var array<string, list<string>> */
    private array $responseTrailers = [];

    private ?\stdClass $finalStatus = null;
    private bool $cancelled = false;
    private ?string $serializedRequest = null;
    private mixed $nativeStream = null;

    /**
     * @param object $argument message instance with serializeToString()
     * @param array<string, string|list<string>> $metadata
     * @param array<string, mixed> $options
     */
    public function start(object $argument, array $metadata = [], array $options = []): void
    {
        $this->mergeStartArgs($metadata, $options);
        $this->serializedRequest = $argument->serializeToString();
    }

    /**
     * @return \Generator<int, object>
     */
    public function responses(): \Generator
    {
        if ($this->cancelled) {
            $this->finalStatus ??= $this->makeStatus(STATUS_CANCELLED, 'call cancelled');
            return;
        }
        if ($this->finalStatus !== null) {
            return;
        }
        if ($this->serializedRequest === null) {
            throw new \RuntimeException('ServerStreamingCall::responses() called before start()');
        }

        yield from $this->responsesNative();
    }

    public function getStatus(): \stdClass
    {
        if ($this->finalStatus === null) {
            throw new \RuntimeException(
                'ServerStreamingCall::getStatus() called before responses() was iterated to completion'
            );
        }
        return $this->finalStatus;
    }

    public function cancel(): void
    {
        $this->cancelled = true;
        $this->finalStatus ??= $this->makeStatus(STATUS_CANCELLED, 'call cancelled');
        if ($this->nativeStream !== null) {
            Internal\NativeTransport::streamCancel($this->nativeStream);
            $this->nativeStream = null;
        }
    }

    /** @return array<string, list<string>> */
    public function getMetadata(): array
    {
        return $this->responseHeaders;
    }

    /** @return array<string, list<string>> */
    public function getTrailingMetadata(): array
    {
        return $this->responseTrailers;
    }

    /**
     * @return \Generator<int, object>
     */
    private function responsesNative(): \Generator
    {
        try {
            $this->nativeStream = Internal\NativeTransport::streamOpen(
                $this->channel->getTarget(),
                $this->method,
                $this->serializedRequest ?? '',
                $this->buildNativeRequestHeaders(),
                isset($this->options['timeout']) ? (int) $this->options['timeout'] : 0,
                $this->channel->credentials,
                $this->maxReceiveMessageLength(),
                $this->authorityOverride(),
            );
        } catch (\RuntimeException $e) {
            if ($e->getMessage() === 'native transport deadline exceeded') {
                $this->finalStatus = $this->makeStatus(STATUS_DEADLINE_EXCEEDED, $e->getMessage());
                return;
            }
            $this->finalStatus = $this->makeStatus(STATUS_UNAVAILABLE, $e->getMessage());
            return;
        }

        try {
            while (true) {
                if ($this->cancelled) {
                    if ($this->nativeStream !== null) {
                        Internal\NativeTransport::streamCancel($this->nativeStream);
                        $this->nativeStream = null;
                    }
                    $this->finalStatus ??= $this->makeStatus(STATUS_CANCELLED, 'call cancelled');
                    return;
                }

                $next = Internal\NativeTransport::streamNext($this->nativeStream);
                if (($next['done'] ?? false) === true) {
                    $this->responseHeaders = $next['headers'] ?? [];
                    $this->responseTrailers = $next['trailers'] ?? [];
                    $this->finalStatus = $this->makeStatus((int) $next['grpc_status'], (string) $next['details']);
                    return;
                }

                yield Internal\Deserialize::apply($this->deserialize, (string) $next['payload']);
            }
        } catch (\RuntimeException $e) {
            $this->finalStatus = $this->makeStatus(STATUS_UNAVAILABLE, $e->getMessage());
            return;
        } finally {
            $this->nativeStream = null;
        }
    }

    private function makeStatus(int $code, string $details): \stdClass
    {
        $s = new \stdClass();
        $s->code = $code;
        $s->details = $details;
        $s->metadata = $this->responseTrailers;
        return $s;
    }
}
