<?php
declare(strict_types=1);

namespace Grpc;

/**
 * A single unary RPC: one request → one response.
 *
 * Constructed by `BaseStub::_simpleRequest`, which immediately invokes
 * `start($argument)`. The caller then blocks on `wait()` to receive the
 * response and status.
 */
class UnaryCall extends AbstractCall
{
    private ?object $diagnostics = null;
    private bool $cancelled = false;
    private ?string $serializedRequest = null;

    /** @var array<string, list<string>> */
    private array $responseHeaders = [];

    /** @var array<string, list<string>> */
    private array $responseTrailers = [];

    /**
     * @param object $argument message instance with serializeToString()
     * @param array<string, string|list<string>> $metadata
     * @param array<string, mixed> $options
     */
    public function start(object $argument, array $metadata = [], array $options = []): void
    {
        $this->mergeStartArgs($metadata, $options);
        $diagnostics = $this->options['php_grpc_lite.diagnostics'] ?? null;
        $this->diagnostics = is_object($diagnostics) ? $diagnostics : null;
        if ($this->diagnostics !== null) {
            $this->diagnostics->events = [];
        }

        $serializeStartedNs = hrtime(true);
        $this->serializedRequest = $argument->serializeToString();
        $this->recordDiagnostic('request_serialize_ns', hrtime(true) - $serializeStartedNs);
        $this->recordDiagnostic('request_payload_bytes', strlen($this->serializedRequest));
        $this->recordDiagnostic('request_frame_bytes', 5 + strlen($this->serializedRequest));
    }

    /**
     * Block until the response arrives. Returns [response, status].
     *
     * @return array{0: object|null, 1: \stdClass}
     */
    public function wait(): array
    {
        if ($this->cancelled) {
            return [null, $this->makeStatus(STATUS_CANCELLED, 'call cancelled')];
        }
        if ($this->serializedRequest === null) {
            throw new \RuntimeException('UnaryCall::wait() called before start()');
        }

        try {
            $headers = $this->buildHttp2RequestHeaders();
            $transportStartedNs = hrtime(true);
            $result = Internal\Http2Transport::unarySimple(
                $this->channel->getTarget(),
                $this->method,
                $this->serializedRequest,
                $headers,
                isset($this->options['timeout']) ? (int) $this->options['timeout'] : 0,
                $this->channel->credentials,
                $this->maxReceiveMessageLength(),
                $this->authorityOverride(),
                $this->tlsVerifyNameOverride(),
            );
            $this->recordDiagnostic('http2_transport_call_ns', hrtime(true) - $transportStartedNs);
        } catch (\RuntimeException $e) {
            if ($e->getMessage() === 'HTTP/2 transport deadline exceeded') {
                return [null, $this->makeStatus(STATUS_DEADLINE_EXCEEDED, $e->getMessage())];
            }
            return [null, $this->makeStatus(STATUS_UNAVAILABLE, $e->getMessage())];
        }

        $normalizeStartedNs = hrtime(true);
        $code = $result['grpc_status'];
        $this->responseHeaders = $result['headers'];
        $this->responseTrailers = $result['trailers'];
        $payload = $result['payloads'][0] ?? null;
        $this->recordDiagnostic('http2_result_normalize_ns', hrtime(true) - $normalizeStartedNs);
        $this->recordHttp2RawDiagnostics($result['raw']);

        $response = null;
        if ($payload !== null && $this->deserialize !== null) {
            $deserializeStartedNs = hrtime(true);
            $response = Internal\Deserialize::apply($this->deserialize, $payload);
            $this->recordDiagnostic('http2_response_deserialize_ns', hrtime(true) - $deserializeStartedNs);
        }

        $statusStartedNs = hrtime(true);
        $status = $this->makeStatus($code, $result['details']);
        $this->recordDiagnostic('http2_status_build_ns', hrtime(true) - $statusStartedNs);
        return [$response, $status];
    }

    /** @param array<string, mixed> $raw */
    private function recordHttp2RawDiagnostics(array $raw): void
    {
        foreach ([
            'total_us',
            'connect_us',
            'setup_us',
            'submit_us',
            'initial_send_us',
            'recv_loop_us',
            'cleanup_us',
            'bytes_sent',
            'bytes_received',
            'sent_frames',
            'recv_frames',
        ] as $name) {
            $value = $raw[$name] ?? null;
            if (is_int($value) || is_float($value)) {
                $this->recordDiagnostic('http2_raw_' . $name, str_ends_with($name, '_us') ? $value * 1000 : $value);
            }
        }
    }

    public function cancel(): void
    {
        $this->cancelled = true;
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

    private function makeStatus(int $code, string $details): \stdClass
    {
        $s = new \stdClass();
        $s->code = $code;
        $s->details = $details;
        $s->metadata = $this->responseTrailers;
        return $s;
    }

    private function recordDiagnostic(string $name, int|float $value, bool $add = false): void
    {
        if ($this->diagnostics === null) {
            return;
        }

        if ($add) {
            $this->diagnostics->{$name} = ($this->diagnostics->{$name} ?? 0) + $value;
            return;
        }

        $this->diagnostics->{$name} = $value;
    }
}
