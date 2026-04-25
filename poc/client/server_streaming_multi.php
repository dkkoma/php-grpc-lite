<?php
declare(strict_types=1);

/**
 * PoC spike: server-streaming via curl_multi to expose a true
 * incremental Generator.
 *
 * Compare with server_streaming.php (blocking via curl_exec) — the question
 * is whether responses() can yield each frame as it arrives, allowing the
 * caller to start processing message N while N+1 is still in flight.
 *
 * Key trick: WRITEFUNCTION cannot yield (it runs inside libcurl). Instead it
 * pushes complete frames into a queue. The Generator wraps the loop:
 *   curl_multi_exec → drain queue → yield → curl_multi_select → repeat.
 *
 * Usage:
 *   php poc/client/server_streaming_multi.php [name]
 */

require __DIR__ . '/lib.php';

const HOST = 'test-server';
const PORT = 50051;
const SERVICE_METHOD = '/helloworld.Greeter/SayManyHellos';

/**
 * Returns a Generator that yields HelloReply messages as they arrive.
 *
 * @return Generator<int, string>  yields the decoded `message` field of each HelloReply
 */
function call_say_many_hellos(string $name): Generator
{
    $startTime = hrtime(true);
    $elapsed = fn() => (hrtime(true) - $startTime) / 1e6;

    $frame = grpc_frame(pb_encode_string_field(1, $name));

    $buffer = '';
    /** @var string[] $pending complete frame payloads waiting to be yielded */
    $pending = [];
    $headers = [];

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
        CURLOPT_HEADERFUNCTION => function ($ch, string $line) use (&$headers, $elapsed): int {
            $trim = rtrim($line, "\r\n");
            if ($trim !== '' && !str_starts_with($trim, 'HTTP/')) {
                [$k, $v] = array_pad(explode(':', $trim, 2), 2, '');
                $headers[strtolower(trim($k))][] = ltrim($v);
                fprintf(STDERR, "[%7.2f ms] header: %s\n", $elapsed(), $trim);
            }
            return strlen($line);
        },
        CURLOPT_WRITEFUNCTION  => function ($ch, string $chunk) use (&$buffer, &$pending, $elapsed): int {
            fprintf(STDERR, "[%7.2f ms] write callback: %d bytes\n", $elapsed(), strlen($chunk));
            $buffer .= $chunk;
            while (strlen($buffer) >= 5) {
                $len = unpack('N', substr($buffer, 1, 4))[1];
                if (strlen($buffer) < 5 + $len) {
                    break;
                }
                $pending[] = substr($buffer, 5, $len);
                $buffer = substr($buffer, 5 + $len);
            }
            return strlen($chunk);
        },
    ]);

    $mh = curl_multi_init();
    curl_multi_add_handle($mh, $ch);

    try {
        do {
            curl_multi_exec($mh, $running);

            // Drain anything that completed in this round.
            while ($pending !== []) {
                $payload = array_shift($pending);
                fprintf(STDERR, "[%7.2f ms]   yield to caller\n", $elapsed());
                yield decode_hello_reply($payload);
            }

            if ($running > 0) {
                // Block until I/O is ready, but bound the wait so we don't
                // hang on misbehaving servers. 1 sec is plenty for I/O wakeup.
                curl_multi_select($mh, 1.0);
            }
        } while ($running > 0);

        // Drain any frames that arrived in the final pump (after $running hit 0
        // but before we drained).
        while ($pending !== []) {
            yield decode_hello_reply(array_shift($pending));
        }

        $info = curl_multi_info_read($mh);
        if ($info && $info['result'] !== CURLE_OK) {
            throw new RuntimeException("curl error: " . curl_strerror($info['result']));
        }

        $status = (int)($headers['grpc-status'][0] ?? -1);
        $message = $headers['grpc-message'][0] ?? '';
        if ($status !== 0) {
            throw new RuntimeException("gRPC error: code=$status message=$message");
        }
        fprintf(STDERR, "[%7.2f ms] stream complete (grpc-status=0)\n", $elapsed());
    } finally {
        curl_multi_remove_handle($mh, $ch);
        curl_close($ch);
        curl_multi_close($mh);
    }
}

// --- Driver: simulate caller-side incremental processing --------------------
$name = $argv[1] ?? 'World';
$start = hrtime(true);
$rel = fn() => (hrtime(true) - $start) / 1e6;

$count = 0;
foreach (call_say_many_hellos($name) as $message) {
    $count++;
    printf("[%7.2f ms] caller received: %s\n", $rel(), $message);
    // Emulate caller-side work between yields. If yields are truly incremental,
    // total time grows; if blocking, this work doesn't change anything.
    usleep(10_000);  // 10ms of "processing"
}
printf("\n=== summary ===\n  yielded %d messages in %.2f ms total\n", $count, $rel());
