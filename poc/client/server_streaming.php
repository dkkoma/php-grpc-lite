<?php
declare(strict_types=1);

/**
 * PoC: server-streaming RPC using raw libcurl + HTTP/2 prior knowledge.
 *
 * Validates that:
 *  - CURLOPT_WRITEFUNCTION is invoked incrementally as the server emits each
 *    message (server sleeps 100ms between sends).
 *  - Multiple gRPC frames can be parsed from the chunked body, including
 *    frames that may straddle write-callback boundaries.
 *  - HTTP/2 trailers (grpc-status / grpc-message) arrive AFTER all body
 *    chunks, but flow into the same CURLOPT_HEADERFUNCTION channel.
 *
 * Usage (from inside the dev container):
 *   php poc/client/server_streaming.php [name]
 */

require __DIR__ . '/lib.php';

const HOST = 'test-server';
const PORT = 50051;
const SERVICE_METHOD = '/helloworld.Greeter/SayManyHellos';

$name = $argv[1] ?? 'World';
$frame = grpc_frame(pb_encode_string_field(1, $name));

$startTime = hrtime(true);
$headers = [];
$buffer = '';
$frames = [];

function elapsed_ms(int $startTime): float
{
    return (hrtime(true) - $startTime) / 1e6;
}

$ch = curl_init();
curl_setopt_array($ch, [
    CURLOPT_URL            => sprintf('http://%s:%d%s', HOST, PORT, SERVICE_METHOD),
    CURLOPT_HTTP_VERSION   => CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE,
    CURLOPT_POST           => true,
    CURLOPT_POSTFIELDS     => $frame,
    CURLOPT_HTTPHEADER     => [
        'Content-Type: application/grpc',
        'TE: trailers',
        'User-Agent: php-grpc-lite-poc/0.0.1',
    ],
    CURLOPT_RETURNTRANSFER => true,

    CURLOPT_HEADERFUNCTION => function ($ch, string $line) use (&$headers, $startTime): int {
        $trim = rtrim($line, "\r\n");
        if ($trim !== '') {
            printf("[%7.2f ms] header: %s\n", elapsed_ms($startTime), $trim);
            if (!str_starts_with($trim, 'HTTP/')) {
                [$k, $v] = array_pad(explode(':', $trim, 2), 2, '');
                $headers[strtolower(trim($k))][] = ltrim($v);
            }
        }
        return strlen($line);
    },

    CURLOPT_WRITEFUNCTION  => function ($ch, string $chunk) use (&$buffer, &$frames, $startTime): int {
        printf("[%7.2f ms] write callback: %d bytes received\n", elapsed_ms($startTime), strlen($chunk));
        $buffer .= $chunk;

        // Try to extract as many complete frames as the buffer holds.
        while (strlen($buffer) >= 5) {
            $flag = ord($buffer[0]);
            $len  = unpack('N', substr($buffer, 1, 4))[1];
            if (strlen($buffer) < 5 + $len) {
                printf("[%7.2f ms]   partial frame in buffer: have=%d need=%d\n",
                       elapsed_ms($startTime), strlen($buffer), 5 + $len);
                break;
            }
            $payload = substr($buffer, 5, $len);
            $buffer  = substr($buffer, 5 + $len);
            $frames[] = $payload;
            $message = decode_hello_reply($payload);
            printf("[%7.2f ms]   frame #%d (flag=%d len=%d): %s\n",
                   elapsed_ms($startTime), count($frames), $flag, $len, $message);
        }
        return strlen($chunk);
    },
]);

$ok = curl_exec($ch);
$err = curl_error($ch);
$httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
curl_close($ch);

if ($ok === false) {
    fwrite(STDERR, "curl error: $err\n");
    exit(1);
}

printf("\n=== summary ===\n");
printf("  HTTP status:    %d\n", $httpCode);
printf("  total frames:   %d\n", count($frames));
printf("  buffer leftover: %d bytes\n", strlen($buffer));
printf("  grpc-status:    %s\n", $headers['grpc-status'][0] ?? 'MISSING');
printf("  grpc-message:   %s\n", $headers['grpc-message'][0] ?? '');
printf("  total elapsed:  %.2f ms\n", elapsed_ms($startTime));

$grpcStatus = (int)($headers['grpc-status'][0] ?? -1);
exit($grpcStatus === 0 ? 0 : 1);
