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
    private string $body = '';
    private bool $bodyStarted = false;

    /** @var array<string, list<string>> */
    private array $responseHeaders = [];

    /** @var array<string, list<string>> */
    private array $responseTrailers = [];

    /**
     * Set up the libcurl request. The actual I/O happens in wait().
     *
     * @param object $argument message instance with serializeToString()
     * @param array<string, string|list<string>> $metadata
     * @param array<string, mixed> $options
     */
    public function start(object $argument, array $metadata = [], array $options = []): void
    {
        $this->mergeStartArgs($metadata, $options);

        $serialized = $argument->serializeToString();
        $frame = "\x00" . pack('N', strlen($serialized)) . $serialized;

        $this->ch = $this->initCurl();
        curl_setopt_array($this->ch, [
            CURLOPT_URL            => $this->buildUrl(),
            CURLOPT_HTTP_VERSION   => $this->getHttpVersion(),
            CURLOPT_POST           => true,
            CURLOPT_POSTFIELDS     => $frame,
            CURLOPT_HTTPHEADER     => $this->buildRequestHeaders(),
            CURLOPT_RETURNTRANSFER => true,
            CURLOPT_HEADERFUNCTION => $this->onHeader(...),
            CURLOPT_WRITEFUNCTION  => $this->onBodyChunk(...),
        ]);
        $this->applyTlsOptions($this->ch);
        $this->applyTimeoutOptions($this->ch);
    }

    /**
     * Block until the response arrives. Returns [response, status].
     *
     * @return array{0: object|null, 1: \stdClass}
     */
    public function wait(): array
    {
        if ($this->ch === null) {
            throw new \RuntimeException('UnaryCall::wait() called before start()');
        }

        $ch = $this->ch;
        curl_exec($ch);
        $errno = curl_errno($ch);
        $errMsg = curl_error($ch);
        $this->ch = null;

        if ($errno !== 0) {
            $this->discardCurl($ch);
            $code = $errno === CURLE_OPERATION_TIMEDOUT
                ? STATUS_DEADLINE_EXCEEDED
                : STATUS_UNAVAILABLE;
            return [null, $this->makeStatus($code, "curl error ($errno): $errMsg")];
        }

        $this->releaseCurl($ch);
        return [$this->parseResponseFrame(), $this->buildStatusFromTrailers()];
    }

    public function cancel(): void
    {
        if ($this->ch !== null) {
            $this->discardCurl($this->ch);
            $this->ch = null;
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

    private function parseResponseFrame(): ?object
    {
        if (strlen($this->body) < 5) {
            return null;
        }
        $len = unpack('N', substr($this->body, 1, 4))[1];
        $payload = substr($this->body, 5, $len);
        if ($this->deserialize === null) {
            return null;
        }
        return Internal\Deserialize::apply($this->deserialize, $payload);
    }

    private function buildStatusFromTrailers(): \stdClass
    {
        $code = isset($this->responseTrailers['grpc-status'])
            ? (int) $this->responseTrailers['grpc-status'][0]
            : STATUS_UNKNOWN;
        $message = $this->responseTrailers['grpc-message'][0] ?? '';
        return $this->makeStatus($code, $message);
    }

    private function makeStatus(int $code, string $details): \stdClass
    {
        $s = new \stdClass();
        $s->code = $code;
        $s->details = $details;
        $s->metadata = $this->responseTrailers;
        return $s;
    }

    private function onHeader(\CurlHandle $ch, string $line): int
    {
        $trim = rtrim($line, "\r\n");
        if ($trim !== '' && !str_starts_with($trim, 'HTTP/')) {
            [$k, $v] = array_pad(explode(':', $trim, 2), 2, '');
            $key = strtolower(trim($k));
            $val = ltrim($v);
            if ($this->isStatusHeader($key)) {
                $this->responseTrailers[$key][] = $val;
            } elseif (!$this->bodyStarted) {
                $this->responseHeaders[$key][] = $val;
            } else {
                $this->responseTrailers[$key][] = $val;
            }
        }
        return strlen($line);
    }

    private function isStatusHeader(string $key): bool
    {
        return $key === 'grpc-status'
            || $key === 'grpc-message'
            || $key === 'grpc-status-details-bin';
    }

    private function onBodyChunk(\CurlHandle $ch, string $chunk): int
    {
        $this->bodyStarted = true;
        $this->body .= $chunk;
        return strlen($chunk);
    }
}
