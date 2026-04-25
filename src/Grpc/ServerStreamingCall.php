<?php
declare(strict_types=1);

namespace Grpc;

/**
 * One request → stream of responses. Constructed via
 * `BaseStub::_serverStreamRequest`, which calls `start($argument)` to
 * stage the libcurl handle. The caller then iterates `responses()` —
 * a Generator that yields each decoded message as soon as the
 * corresponding HTTP/2 DATA frame arrives.
 *
 * The Generator owns the curl_multi pump: it advances I/O, drains any
 * complete gRPC frames into yields, and finally collects trailers into a
 * status object exposed via `getStatus()`.
 */
class ServerStreamingCall extends AbstractCall
{
    private string $buffer = '';
    private bool $bodyStarted = false;

    /** @var list<string> complete gRPC frame payloads waiting to be yielded */
    private array $pending = [];

    /** @var array<string, list<string>> */
    private array $responseHeaders = [];

    /** @var array<string, list<string>> */
    private array $responseTrailers = [];

    private ?\stdClass $finalStatus = null;

    /**
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
    }

    /**
     * @return \Generator<int, object>
     */
    public function responses(): \Generator
    {
        if ($this->ch === null) {
            throw new \RuntimeException('ServerStreamingCall::responses() called before start()');
        }
        if ($this->finalStatus !== null) {
            // already drained — cannot iterate twice
            return;
        }

        $mh = curl_multi_init();
        curl_multi_add_handle($mh, $this->ch);
        $errCode = null;

        try {
            do {
                curl_multi_exec($mh, $running);

                while ($info = curl_multi_info_read($mh)) {
                    if ($info['result'] !== CURLE_OK) {
                        $errCode = $info['result'];
                    }
                }

                while ($this->pending !== []) {
                    $payload = array_shift($this->pending);
                    yield Internal\Deserialize::apply($this->deserialize, $payload);
                }

                if ($running > 0) {
                    curl_multi_select($mh, 1.0);
                }
            } while ($running > 0);

            while ($this->pending !== []) {
                $payload = array_shift($this->pending);
                yield Internal\Deserialize::apply($this->deserialize, $payload);
            }

            if ($errCode !== null && $errCode !== CURLE_OK) {
                $this->finalStatus = $this->makeStatus(
                    STATUS_UNAVAILABLE,
                    "curl error ($errCode): " . curl_strerror($errCode),
                );
            } else {
                $this->finalStatus = $this->buildStatusFromTrailers();
            }
        } finally {
            curl_multi_remove_handle($mh, $this->ch);
            curl_close($this->ch);
            $this->ch = null;
            curl_multi_close($mh);

            if ($this->finalStatus === null) {
                $this->finalStatus = $this->buildStatusFromTrailers();
            }
        }
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
        if ($this->ch !== null) {
            curl_close($this->ch);
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
            if (!$this->bodyStarted) {
                $this->responseHeaders[$key][] = $val;
            } else {
                $this->responseTrailers[$key][] = $val;
            }
        }
        return strlen($line);
    }

    private function onBodyChunk(\CurlHandle $ch, string $chunk): int
    {
        $this->bodyStarted = true;
        $this->buffer .= $chunk;

        while (strlen($this->buffer) >= 5) {
            $len = unpack('N', substr($this->buffer, 1, 4))[1];
            if (strlen($this->buffer) < 5 + $len) {
                break;
            }
            $this->pending[] = substr($this->buffer, 5, $len);
            $this->buffer = substr($this->buffer, 5 + $len);
        }
        return strlen($chunk);
    }
}
