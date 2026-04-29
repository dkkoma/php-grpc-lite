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
    private ?object $diagnostics = null;

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
        $diagnostics = $this->options['php_grpc_lite.diagnostics'] ?? null;
        $this->diagnostics = is_object($diagnostics) ? $diagnostics : null;
        if ($this->diagnostics !== null) {
            $this->diagnostics->events = [];
        }

        $serializeStartedNs = hrtime(true);
        $serialized = $argument->serializeToString();
        $this->recordDiagnostic('request_serialize_ns', hrtime(true) - $serializeStartedNs);

        $frameStartedNs = hrtime(true);
        $frame = "\x00" . pack('N', strlen($serialized)) . $serialized;
        $this->recordDiagnostic('request_frame_build_ns', hrtime(true) - $frameStartedNs);
        $this->recordDiagnostic('request_payload_bytes', strlen($serialized));
        $this->recordDiagnostic('request_frame_bytes', strlen($frame));

        $this->ch = $this->initCurl();
        $headersStartedNs = hrtime(true);
        $requestHeaders = $this->buildRequestHeaders();
        $this->recordDiagnostic('request_header_build_ns', hrtime(true) - $headersStartedNs);
        $this->recordDiagnostic('request_headers_total', count($requestHeaders));
        curl_setopt_array($this->ch, [
            CURLOPT_URL            => $this->buildUrl(),
            CURLOPT_HTTP_VERSION   => $this->getHttpVersion(),
            CURLOPT_POST           => true,
            CURLOPT_POSTFIELDS     => $frame,
            CURLOPT_HTTPHEADER     => $requestHeaders,
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
        $curlStartedNs = hrtime(true);
        curl_exec($ch);
        $this->recordDiagnostic('curl_exec_ns', hrtime(true) - $curlStartedNs);
        $this->recordCurlTimingDiagnostics($ch);
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

        $validateStartedNs = hrtime(true);
        $protocolStatus = $this->validateGrpcResponse($ch);
        $this->recordDiagnostic('validate_response_ns', hrtime(true) - $validateStartedNs);
        $this->releaseCurl($ch);
        if ($protocolStatus !== null) {
            return [null, $protocolStatus];
        }
        $compressionStartedNs = hrtime(true);
        $compressionStatus = $this->validateCompression();
        $this->recordDiagnostic('validate_compression_ns', hrtime(true) - $compressionStartedNs);
        if ($compressionStatus !== null) {
            return [null, $compressionStatus];
        }
        $response = $this->parseResponseFrame();
        $statusStartedNs = hrtime(true);
        $status = $this->buildStatusFromTrailers();
        $this->recordDiagnostic('status_build_ns', hrtime(true) - $statusStartedNs);
        return [$response, $status];
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
        $lengthStartedNs = hrtime(true);
        $len = unpack('N', substr($this->body, 1, 4))[1];
        $this->recordDiagnostic('response_frame_length_ns', hrtime(true) - $lengthStartedNs);
        $this->recordDiagnostic('response_body_bytes', strlen($this->body));
        $this->recordDiagnostic('response_payload_bytes', $len);

        $sliceStartedNs = hrtime(true);
        $payload = substr($this->body, 5, $len);
        $this->recordDiagnostic('response_payload_slice_ns', hrtime(true) - $sliceStartedNs);
        if ($this->deserialize === null) {
            return null;
        }
        $deserializeStartedNs = hrtime(true);
        $response = Internal\Deserialize::apply($this->deserialize, $payload);
        $this->recordDiagnostic('response_deserialize_ns', hrtime(true) - $deserializeStartedNs);
        return $response;
    }

    private function validateCompression(): ?\stdClass
    {
        $encoding = $this->findUnsupportedGrpcEncoding($this->responseHeaders);
        if ($encoding !== null) {
            return $this->makeStatus(
                STATUS_UNIMPLEMENTED,
                "unsupported grpc-encoding: $encoding",
            );
        }

        if ($this->body !== '' && ord($this->body[0]) !== 0) {
            return $this->makeStatus(
                STATUS_UNIMPLEMENTED,
                'compressed gRPC messages are not supported',
            );
        }

        return null;
    }

    private function buildStatusFromTrailers(): \stdClass
    {
        $code = isset($this->responseTrailers['grpc-status'])
            ? (int) $this->responseTrailers['grpc-status'][0]
            : STATUS_UNKNOWN;
        $message = $this->decodeGrpcMessage($this->responseTrailers['grpc-message'][0] ?? '');
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

    private function validateGrpcResponse(\CurlHandle $ch): ?\stdClass
    {
        if (isset($this->responseTrailers['grpc-status'])) {
            return null;
        }

        $httpStatus = (int) curl_getinfo($ch, CURLINFO_RESPONSE_CODE);
        if ($httpStatus !== 200) {
            return $this->makeStatus(
                $this->mapHttpStatusToGrpcStatus($httpStatus),
                "HTTP status $httpStatus without grpc-status",
            );
        }

        $contentType = strtolower($this->responseHeaders['content-type'][0] ?? '');
        if (!str_starts_with($contentType, 'application/grpc')) {
            return $this->makeStatus(
                STATUS_UNKNOWN,
                "invalid gRPC content-type: " . ($contentType === '' ? '<missing>' : $contentType),
            );
        }

        return null;
    }

    private function onHeader(\CurlHandle $ch, string $line): int
    {
        $startedNs = hrtime(true);
        $trim = rtrim($line, "\r\n");
        if ($trim !== '' && !str_starts_with($trim, 'HTTP/')) {
            [$k, $v] = array_pad(explode(':', $trim, 2), 2, '');
            $key = strtolower(trim($k));
            $val = ltrim($v);
            if ($this->isStatusHeader($key)) {
                $this->appendMetadataHeader($this->responseTrailers, $key, $val);
            } elseif (!$this->bodyStarted) {
                $this->appendMetadataHeader($this->responseHeaders, $key, $val);
            } else {
                $this->appendMetadataHeader($this->responseTrailers, $key, $val);
            }
        }
        $this->recordDiagnostic('header_callback_ns_total', hrtime(true) - $startedNs, true);
        $this->recordDiagnostic('header_lines_total', 1, true);
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
        $startedNs = hrtime(true);
        $chunkBytes = strlen($chunk);
        $this->bodyStarted = true;
        $this->body .= $chunk;
        $appendNs = hrtime(true) - $startedNs;
        $this->recordDiagnostic('body_append_ns_total', $appendNs, true);
        $this->recordDiagnostic('body_chunks_total', 1, true);
        $this->recordDiagnostic('body_chunk_bytes_total', $chunkBytes, true);
        $this->recordDiagnosticMax('body_chunk_bytes_max', $chunkBytes);
        $this->recordDiagnosticMax('body_append_ns_max', $appendNs);
        return $chunkBytes;
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

    private function recordDiagnosticMax(string $name, int|float $value): void
    {
        if ($this->diagnostics === null) {
            return;
        }

        $this->diagnostics->{$name} = max($this->diagnostics->{$name} ?? 0, $value);
    }

    private function recordCurlTimingDiagnostics(\CurlHandle $ch): void
    {
        if ($this->diagnostics === null) {
            return;
        }

        foreach ($this->curlTimingInfoMap() as $metric => $infoConstant) {
            $value = curl_getinfo($ch, $infoConstant);
            if (is_int($value) || is_float($value)) {
                $this->recordDiagnostic($metric, $value * 1000);
            }
        }
    }

    /**
     * @return array<string, int>
     */
    private function curlTimingInfoMap(): array
    {
        return [
            'curl_total_time_ns' => \CURLINFO_TOTAL_TIME_T,
            'curl_namelookup_time_ns' => \CURLINFO_NAMELOOKUP_TIME_T,
            'curl_connect_time_ns' => \CURLINFO_CONNECT_TIME_T,
            'curl_appconnect_time_ns' => \CURLINFO_APPCONNECT_TIME_T,
            'curl_pretransfer_time_ns' => \CURLINFO_PRETRANSFER_TIME_T,
            'curl_starttransfer_time_ns' => \CURLINFO_STARTTRANSFER_TIME_T,
            'curl_redirect_time_ns' => \CURLINFO_REDIRECT_TIME_T,
        ];
    }
}
