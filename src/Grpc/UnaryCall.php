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
    private bool $bodyStarted = false;
    private int $bodyBytes = 0;
    private ?object $diagnostics = null;
    private int $curlExecStartedNs = 0;
    private bool $returnTransferFastPath = false;
    private string $returnedBody = '';

    /** @var list<string> */
    private array $bodyChunks = [];

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
        $this->returnTransferFastPath = ($this->options['php_grpc_lite.return_transfer_fast_path'] ?? false) === true;
        $this->recordDiagnostic('return_transfer_fast_path', $this->returnTransferFastPath ? 1 : 0);
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

        $initCurlStartedNs = hrtime(true);
        $this->ch = $this->initCurl();
        $this->recordDiagnostic('init_curl_ns', hrtime(true) - $initCurlStartedNs);
        $headersStartedNs = hrtime(true);
        $requestHeaders = $this->buildRequestHeaders();
        $this->recordDiagnostic('request_header_build_ns', hrtime(true) - $headersStartedNs);
        $this->recordDiagnostic('request_headers_total', count($requestHeaders));
        $setoptStartedNs = hrtime(true);
        $curlOptions = [
            CURLOPT_URL            => $this->buildUrl(),
            CURLOPT_HTTP_VERSION   => $this->getHttpVersion(),
            CURLOPT_POST           => true,
            CURLOPT_POSTFIELDS     => $frame,
            CURLOPT_HTTPHEADER     => $requestHeaders,
            CURLOPT_RETURNTRANSFER => true,
            CURLOPT_HEADERFUNCTION => $this->onHeader(...),
        ];
        if (!$this->returnTransferFastPath) {
            $curlOptions[CURLOPT_WRITEFUNCTION] = $this->onBodyChunk(...);
        }
        curl_setopt_array($this->ch, $curlOptions);
        $this->recordDiagnostic('curl_setopt_ns', hrtime(true) - $setoptStartedNs);
        $this->applyCurlTraceOptions($this->ch);
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
        $this->curlExecStartedNs = $curlStartedNs;
        $curlResult = curl_exec($ch);
        $this->recordDiagnostic('curl_exec_ns', hrtime(true) - $curlStartedNs);
        if ($this->returnTransferFastPath && is_string($curlResult)) {
            $this->returnedBody = $curlResult;
            $this->bodyBytes = strlen($curlResult);
            $this->bodyStarted = $this->bodyBytes > 0;
            $this->recordDiagnostic('return_transfer_body_bytes', $this->bodyBytes);
        }
        $this->recordCurlTimingDiagnostics($ch);
        $errno = curl_errno($ch);
        $errMsg = curl_error($ch);
        $this->ch = null;
        $afterCurlStartedNs = hrtime(true);

        if ($errno !== 0) {
            $this->discardCurl($ch);
            $this->recordDiagnostic('wait_after_curl_userland_ns', hrtime(true) - $afterCurlStartedNs);
            $code = $errno === CURLE_OPERATION_TIMEDOUT
                ? STATUS_DEADLINE_EXCEEDED
                : STATUS_UNAVAILABLE;
            return [null, $this->makeStatus($code, "curl error ($errno): $errMsg")];
        }

        $validateStartedNs = hrtime(true);
        $protocolStatus = $this->validateGrpcResponse($ch);
        $this->recordDiagnostic('validate_response_ns', hrtime(true) - $validateStartedNs);
        $releaseStartedNs = hrtime(true);
        $this->releaseCurl($ch);
        $this->recordDiagnostic('release_curl_ns', hrtime(true) - $releaseStartedNs);
        if ($protocolStatus !== null) {
            $this->recordDiagnostic('wait_after_curl_userland_ns', hrtime(true) - $afterCurlStartedNs);
            return [null, $protocolStatus];
        }
        $compressionStartedNs = hrtime(true);
        $compressionStatus = $this->validateCompression();
        $this->recordDiagnostic('validate_compression_ns', hrtime(true) - $compressionStartedNs);
        if ($compressionStatus !== null) {
            $this->recordDiagnostic('wait_after_curl_userland_ns', hrtime(true) - $afterCurlStartedNs);
            return [null, $compressionStatus];
        }
        $response = $this->parseResponseFrame();
        $statusStartedNs = hrtime(true);
        $status = $this->buildStatusFromTrailers();
        $this->recordDiagnostic('status_build_ns', hrtime(true) - $statusStartedNs);
        $this->recordDiagnostic('wait_after_curl_userland_ns', hrtime(true) - $afterCurlStartedNs);
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
        if ($this->bodyBytes < 5) {
            return null;
        }
        $body = $this->assembleResponseBody();
        $lengthStartedNs = hrtime(true);
        $len = unpack('N', substr($body, 1, 4))[1];
        $this->recordDiagnostic('response_frame_length_ns', hrtime(true) - $lengthStartedNs);
        $this->recordDiagnostic('response_body_bytes', strlen($body));
        $this->recordDiagnostic('response_payload_bytes', $len);

        $sliceStartedNs = hrtime(true);
        $payload = substr($body, 5, $len);
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

        $firstChunk = $this->bodyChunks[0] ?? '';
        if ($this->returnTransferFastPath) {
            $firstChunk = $this->returnedBody;
        }
        if ($firstChunk !== '' && ord($firstChunk[0]) !== 0) {
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
        $this->recordCallbackOffsetOnce('first_header_callback_ns');
        $trim = rtrim($line, "\r\n");
        if ($trim !== '' && !str_starts_with($trim, 'HTTP/')) {
            [$k, $v] = array_pad(explode(':', $trim, 2), 2, '');
            $key = strtolower(trim($k));
            $val = ltrim($v);
            $appendStartedNs = hrtime(true);
            if ($this->isStatusHeader($key)) {
                $this->recordCallbackOffsetOnce('first_trailer_callback_ns');
                $this->appendMetadataHeader($this->responseTrailers, $key, $val);
                $this->recordDiagnostic('response_trailing_metadata_append_ns_total', hrtime(true) - $appendStartedNs, true);
                $this->recordDiagnostic('response_trailing_metadata_lines_total', 1, true);
            } elseif (!$this->bodyStarted) {
                $this->appendMetadataHeader($this->responseHeaders, $key, $val);
                $this->recordDiagnostic('response_initial_metadata_append_ns_total', hrtime(true) - $appendStartedNs, true);
                $this->recordDiagnostic('response_initial_metadata_lines_total', 1, true);
            } else {
                $this->recordCallbackOffsetOnce('first_trailer_callback_ns');
                $this->appendMetadataHeader($this->responseTrailers, $key, $val);
                $this->recordDiagnostic('response_trailing_metadata_append_ns_total', hrtime(true) - $appendStartedNs, true);
                $this->recordDiagnostic('response_trailing_metadata_lines_total', 1, true);
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
        $this->recordCallbackOffsetOnce('first_body_chunk_callback_ns');
        $chunkBytes = strlen($chunk);
        $this->bodyStarted = true;
        $this->bodyChunks[] = $chunk;
        $this->bodyBytes += $chunkBytes;
        $appendNs = hrtime(true) - $startedNs;
        $this->recordDiagnostic('body_append_ns_total', $appendNs, true);
        $this->recordDiagnostic('body_chunks_total', 1, true);
        $this->recordDiagnostic('body_chunk_bytes_total', $chunkBytes, true);
        $this->recordDiagnosticMax('body_chunk_bytes_max', $chunkBytes);
        $this->recordDiagnosticMax('body_append_ns_max', $appendNs);
        return $chunkBytes;
    }

    private function assembleResponseBody(): string
    {
        if ($this->returnTransferFastPath) {
            return $this->returnedBody;
        }

        if (count($this->bodyChunks) === 1) {
            return $this->bodyChunks[0];
        }

        $startedNs = hrtime(true);
        $body = implode('', $this->bodyChunks);
        $this->recordDiagnostic('response_body_assemble_ns', hrtime(true) - $startedNs);
        return $body;
    }

    private function recordCallbackOffsetOnce(string $name): void
    {
        if ($this->diagnostics === null || $this->curlExecStartedNs === 0 || isset($this->diagnostics->{$name})) {
            return;
        }

        $this->diagnostics->{$name} = hrtime(true) - $this->curlExecStartedNs;
    }

    private function applyCurlTraceOptions(\CurlHandle $ch): void
    {
        $trace = $this->options['php_grpc_lite.curl_trace'] ?? null;
        if (!is_callable($trace)) {
            return;
        }

        curl_setopt_array($ch, [
            CURLOPT_VERBOSE => true,
            CURLOPT_DEBUGFUNCTION => static function (\CurlHandle $handle, int $type, string $data) use ($trace): int {
                $trace($type, $data, hrtime(true));
                return 0;
            },
        ]);
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
        foreach ($this->curlTransferInfoMap() as $metric => $infoConstant) {
            $value = curl_getinfo($ch, $infoConstant);
            if (is_int($value) || is_float($value)) {
                $this->recordDiagnostic($metric, $value);
            }
        }
        $totalTimeNs = $this->diagnostics->curl_total_time_ns ?? null;
        $startTransferTimeNs = $this->diagnostics->curl_starttransfer_time_ns ?? null;
        if ((is_int($totalTimeNs) || is_float($totalTimeNs)) && (is_int($startTransferTimeNs) || is_float($startTransferTimeNs))) {
            $this->recordDiagnostic('curl_download_after_starttransfer_ns', max(0, $totalTimeNs - $startTransferTimeNs));
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

    /**
     * @return array<string, int>
     */
    private function curlTransferInfoMap(): array
    {
        return [
            'curl_size_upload_bytes' => \CURLINFO_SIZE_UPLOAD_T,
            'curl_size_download_bytes' => \CURLINFO_SIZE_DOWNLOAD_T,
            'curl_speed_download_bytes_per_second' => \CURLINFO_SPEED_DOWNLOAD_T,
            'curl_request_size_bytes' => \CURLINFO_REQUEST_SIZE,
            'curl_header_size_bytes' => \CURLINFO_HEADER_SIZE,
            'curl_num_connects_total' => \CURLINFO_NUM_CONNECTS,
            'curl_primary_port' => \CURLINFO_PRIMARY_PORT,
            'curl_local_port' => \CURLINFO_LOCAL_PORT,
        ];
    }
}
