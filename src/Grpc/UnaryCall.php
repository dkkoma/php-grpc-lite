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
    private bool $uploadReadCallback = false;
    private bool $nativeTransport = false;
    private bool $cancelled = false;
    private ?string $nativeSerializedRequest = null;
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
        $this->nativeTransport = $this->shouldUseNativeTransport();
        $this->returnTransferFastPath = ($this->options['php_grpc_lite.return_transfer_fast_path'] ?? false) === true;
        $this->uploadReadCallback = ($this->options['php_grpc_lite.upload_read_callback'] ?? false) === true;
        $this->recordDiagnostic('return_transfer_fast_path', $this->returnTransferFastPath ? 1 : 0);
        $this->recordDiagnostic('upload_read_callback', $this->uploadReadCallback ? 1 : 0);
        if ($this->diagnostics !== null) {
            $this->diagnostics->events = [];
        }

        $serializeStartedNs = hrtime(true);
        $serialized = $argument->serializeToString();
        $this->recordDiagnostic('request_serialize_ns', hrtime(true) - $serializeStartedNs);

        $frameStartedNs = hrtime(true);
        $grpcHeader = "\x00" . pack('N', strlen($serialized));
        $frame = $this->uploadReadCallback ? null : $grpcHeader . $serialized;
        $this->recordDiagnostic('request_frame_build_ns', hrtime(true) - $frameStartedNs);
        $this->recordDiagnostic('request_payload_bytes', strlen($serialized));
        $this->recordDiagnostic('request_frame_bytes', strlen($grpcHeader) + strlen($serialized));

        if ($this->nativeTransport) {
            $this->nativeSerializedRequest = $serialized;
            return;
        }

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
            CURLOPT_HTTPHEADER     => $requestHeaders,
            CURLOPT_RETURNTRANSFER => true,
            CURLOPT_HEADERFUNCTION => $this->onHeader(...),
        ];
        if ($this->uploadReadCallback) {
            $curlOptions[CURLOPT_INFILESIZE] = strlen($grpcHeader) + strlen($serialized);
            $curlOptions[CURLOPT_READFUNCTION] = $this->makeUploadReadCallback($grpcHeader, $serialized);
        } else {
            $curlOptions[CURLOPT_POSTFIELDS] = $frame;
        }
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
            if ($this->nativeTransport && $this->nativeSerializedRequest !== null) {
                return $this->waitNative();
            }
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

    /**
     * @return array{0: object|null, 1: \stdClass}
     */
    private function waitNative(): array
    {
        if ($this->cancelled) {
            return [null, $this->makeStatus(STATUS_CANCELLED, 'call cancelled')];
        }

        if (!$this->channel->credentials->isInsecure()) {
            return [null, $this->makeStatus(STATUS_UNAVAILABLE, 'native transport currently supports insecure h2c only')];
        }

        try {
            $headers = $this->buildNativeRequestHeaders();
            $transportStartedNs = hrtime(true);
            if ($this->shouldUseNativeUnarySimple($headers)) {
                $result = Internal\NativeTransport::unarySimple(
                    $this->channel->getTarget(),
                    $this->method,
                    $this->nativeSerializedRequest ?? '',
                    $headers,
                );
                $this->recordDiagnostic('native_unary_simple', 1);
            } else {
                $result = Internal\NativeTransport::unaryBatch(
                    $this->channel->getTarget(),
                    $this->method,
                    $this->nativeSerializedRequest ?? '',
                    $headers,
                    false,
                    true,
                    isset($this->options['timeout']) ? (int) $this->options['timeout'] : 0,
                );
                $this->recordDiagnostic('native_unary_simple', 0);
            }
            $this->recordDiagnostic('native_transport_call_ns', hrtime(true) - $transportStartedNs);
        } catch (\RuntimeException $e) {
            return [null, $this->makeStatus(STATUS_UNAVAILABLE, $e->getMessage())];
        }

        $normalizeStartedNs = hrtime(true);
        $code = $result['grpc_status'];
        $this->responseTrailers = $result['trailers'];
        $payload = $result['payloads'][0] ?? null;
        $this->recordDiagnostic('native_result_normalize_ns', hrtime(true) - $normalizeStartedNs);
        $this->recordNativeRawDiagnostics($result['raw']);
        $response = null;
        if ($payload !== null && $this->deserialize !== null) {
            $deserializeStartedNs = hrtime(true);
            $response = Internal\Deserialize::apply($this->deserialize, $payload);
            $this->recordDiagnostic('native_response_deserialize_ns', hrtime(true) - $deserializeStartedNs);
        }

        $statusStartedNs = hrtime(true);
        $status = $this->makeStatus($code, $result['details']);
        $this->recordDiagnostic('native_status_build_ns', hrtime(true) - $statusStartedNs);
        return [$response, $status];
    }

    /** @param array<string, mixed> $raw */
    private function recordNativeRawDiagnostics(array $raw): void
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
                $this->recordDiagnostic('native_raw_' . $name, str_ends_with($name, '_us') ? $value * 1000 : $value);
            }
        }
    }

    /** @param array<string, string> $headers */
    private function shouldUseNativeUnarySimple(array $headers): bool
    {
        $mode = (string) ($this->options['php_grpc_lite.native_unary_mode']
            ?? $this->channel->opts['php_grpc_lite.native_unary_mode']
            ?? 'simple');
        if ($mode === 'batch') {
            return false;
        }
        if ($mode !== 'simple') {
            throw new \InvalidArgumentException("php_grpc_lite.native_unary_mode must be 'simple' or 'batch'");
        }
        if (isset($this->options['timeout'])) {
            return false;
        }
        foreach (['x-bench-server-stats', 'x-bench-server-timing'] as $diagnosticHeader) {
            if (array_key_exists($diagnosticHeader, $headers)) {
                return false;
            }
        }
        return true;
    }

    public function cancel(): void
    {
        $this->cancelled = true;
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

    /**
     * @return callable(\CurlHandle, resource|null, int): string
     */
    private function makeUploadReadCallback(string $grpcHeader, string $serialized): callable
    {
        $offset = 0;
        $headerBytes = strlen($grpcHeader);
        $totalBytes = $headerBytes + strlen($serialized);

        return function (\CurlHandle $ch, mixed $file, int $length) use ($grpcHeader, $serialized, &$offset, $headerBytes, $totalBytes): string {
            $startedNs = hrtime(true);
            $this->recordCallbackOffsetOnce('upload_first_read_callback_ns');
            $this->recordDiagnostic('upload_read_callback_calls_total', 1, true);
            $this->recordDiagnosticMax('upload_read_callback_requested_bytes_max', $length);

            if ($offset >= $totalBytes || $length <= 0) {
                $this->recordCallbackOffsetOnce('upload_read_complete_ns');
                return '';
            }

            $remaining = min($length, $totalBytes - $offset);
            if ($offset < $headerBytes) {
                $chunk = substr($grpcHeader, $offset, min($remaining, $headerBytes - $offset));
                if (strlen($chunk) < $remaining) {
                    $payloadOffset = 0;
                    $chunk .= substr($serialized, $payloadOffset, $remaining - strlen($chunk));
                }
            } else {
                $payloadOffset = $offset - $headerBytes;
                $chunk = substr($serialized, $payloadOffset, $remaining);
            }

            $chunkBytes = strlen($chunk);
            $offset += $chunkBytes;
            $elapsedNs = hrtime(true) - $startedNs;
            $this->recordDiagnostic('upload_read_callback_ns_total', $elapsedNs, true);
            $this->recordDiagnostic('upload_read_callback_bytes_total', $chunkBytes, true);
            $this->recordDiagnosticMax('upload_read_callback_bytes_max', $chunkBytes);
            $this->recordDiagnosticMax('upload_read_callback_ns_max', $elapsedNs);
            $this->recordCallbackOffset('upload_last_read_callback_ns');
            if ($offset >= $totalBytes) {
                $this->recordCallbackOffsetOnce('upload_read_complete_ns');
            }

            return $chunk;
        };
    }

    private function recordCallbackOffsetOnce(string $name): void
    {
        if ($this->diagnostics === null || $this->curlExecStartedNs === 0 || isset($this->diagnostics->{$name})) {
            return;
        }

        $this->diagnostics->{$name} = hrtime(true) - $this->curlExecStartedNs;
    }

    private function recordCallbackOffset(string $name): void
    {
        if ($this->diagnostics === null || $this->curlExecStartedNs === 0) {
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
        $postTransferTimeNs = $this->diagnostics->curl_posttransfer_time_ns ?? null;
        if ((is_int($postTransferTimeNs) || is_float($postTransferTimeNs)) && (is_int($startTransferTimeNs) || is_float($startTransferTimeNs))) {
            $this->recordDiagnostic('curl_wait_after_upload_before_first_byte_ns', max(0, $startTransferTimeNs - $postTransferTimeNs));
        }
        if ((is_int($postTransferTimeNs) || is_float($postTransferTimeNs)) && (is_int($totalTimeNs) || is_float($totalTimeNs))) {
            $this->recordDiagnostic('curl_after_upload_until_done_ns', max(0, $totalTimeNs - $postTransferTimeNs));
        }
    }

    /**
     * @return array<string, int>
     */
    private function curlTimingInfoMap(): array
    {
        $map = [
            'curl_total_time_ns' => \CURLINFO_TOTAL_TIME_T,
            'curl_namelookup_time_ns' => \CURLINFO_NAMELOOKUP_TIME_T,
            'curl_connect_time_ns' => \CURLINFO_CONNECT_TIME_T,
            'curl_appconnect_time_ns' => \CURLINFO_APPCONNECT_TIME_T,
            'curl_pretransfer_time_ns' => \CURLINFO_PRETRANSFER_TIME_T,
            'curl_starttransfer_time_ns' => \CURLINFO_STARTTRANSFER_TIME_T,
            'curl_redirect_time_ns' => \CURLINFO_REDIRECT_TIME_T,
        ];
        if (defined('CURLINFO_POSTTRANSFER_TIME_T')) {
            $map['curl_posttransfer_time_ns'] = \CURLINFO_POSTTRANSFER_TIME_T;
        }

        return $map;
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
