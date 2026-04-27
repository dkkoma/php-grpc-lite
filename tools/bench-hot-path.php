<?php
declare(strict_types=1);

require __DIR__ . '/../vendor/autoload.php';

use Helloworld\BenchReply;
use Helloworld\HelloRequest;

/**
 * Split the already-warm client path into local CPU costs without touching the
 * network. Keep this outside bench/ so the regular phpbench compare remains
 * focused on php-grpc-lite vs official ext-grpc RPC behavior.
 */

gc_disable();

/** @var list<array{name: string, revs: int, subject: callable(): void}> $cases */
$cases = [];

$helloRequest = new HelloRequest();
$helloRequest->setName('bench');

$cases[] = [
    'name' => 'request serialize + grpc frame',
    'revs' => 200_000,
    'subject' => static function () use ($helloRequest): void {
        $serialized = $helloRequest->serializeToString();
        "\x00" . pack('N', strlen($serialized)) . $serialized;
    },
];

foreach ([0, 100, 1024, 10 * 1024, 100 * 1024] as $bytes) {
    $reply = new BenchReply();
    $reply->setPayload(str_repeat("\0", $bytes));
    $payload = $reply->serializeToString();
    $frame = "\x00" . pack('N', strlen($payload)) . $payload;
    $revs = $bytes >= 100 * 1024 ? 10_000 : 100_000;

    $cases[] = [
        'name' => sprintf('response frame parse only %s', formatBytes($bytes)),
        'revs' => $revs,
        'subject' => static function () use ($frame): void {
            $len = unpack('N', substr($frame, 1, 4))[1];
            substr($frame, 5, $len);
        },
    ];

    $cases[] = [
        'name' => sprintf('response frame parse + protobuf merge %s', formatBytes($bytes)),
        'revs' => $revs,
        'subject' => static function () use ($frame): void {
            $len = unpack('N', substr($frame, 1, 4))[1];
            $payload = substr($frame, 5, $len);
            $message = new BenchReply();
            $message->mergeFromString($payload);
        },
    ];
}

$headers = [
    "HTTP/2 200\r\n",
    "content-type: application/grpc\r\n",
    "grpc-accept-encoding: identity, deflate, gzip\r\n",
    "\r\n",
    "grpc-status: 0\r\n",
    "grpc-message: \r\n",
];

$cases[] = [
    'name' => 'header/trailer line parse x6',
    'revs' => 200_000,
    'subject' => static function () use ($headers): void {
        $bodyStarted = false;
        $responseHeaders = [];
        $responseTrailers = [];

        foreach ($headers as $line) {
            $trim = rtrim($line, "\r\n");
            if ($trim !== '' && !str_starts_with($trim, 'HTTP/')) {
                [$k, $v] = array_pad(explode(':', $trim, 2), 2, '');
                $key = strtolower(trim($k));
                $val = ltrim($v);
                if (!$bodyStarted) {
                    $responseHeaders[$key][] = $val;
                } else {
                    $responseTrailers[$key][] = $val;
                }
            } elseif ($trim === '') {
                $bodyStarted = true;
            }
        }
    },
];

foreach ([100, 1000] as $messageCount) {
    $streamReply = new BenchReply();
    $streamReply->setPayload(str_repeat("\0", 100));
    $streamPayload = $streamReply->serializeToString();
    $streamFrame = "\x00" . pack('N', strlen($streamPayload)) . $streamPayload;
    $streamBuffer = str_repeat($streamFrame, $messageCount);
    $revs = $messageCount === 100 ? 20_000 : 2_000;

    $cases[] = [
        'name' => "stream split current $messageCount messages",
        'revs' => $revs,
        'subject' => static function () use ($streamBuffer): void {
            $buffer = $streamBuffer;
            $pending = [];
            while (strlen($buffer) >= 5) {
                $len = unpack('N', substr($buffer, 1, 4))[1];
                if (strlen($buffer) < 5 + $len) {
                    break;
                }
                $pending[] = substr($buffer, 5, $len);
                $buffer = substr($buffer, 5 + $len);
            }
        },
    ];

    $cases[] = [
        'name' => "stream split offset $messageCount messages",
        'revs' => $revs,
        'subject' => static function () use ($streamBuffer): void {
            $bufferLength = strlen($streamBuffer);
            $offset = 0;
            $pending = [];
            while ($bufferLength - $offset >= 5) {
                $len = unpack('N', substr($streamBuffer, $offset + 1, 4))[1];
                if ($bufferLength - $offset < 5 + $len) {
                    break;
                }
                $pending[] = substr($streamBuffer, $offset + 5, $len);
                $offset += 5 + $len;
            }
        },
    ];

    $cases[] = [
        'name' => "stream split current + merge $messageCount messages",
        'revs' => (int) max(1_000, $revs / 2),
        'subject' => static function () use ($streamBuffer): void {
            $buffer = $streamBuffer;
            while (strlen($buffer) >= 5) {
                $len = unpack('N', substr($buffer, 1, 4))[1];
                if (strlen($buffer) < 5 + $len) {
                    break;
                }
                $payload = substr($buffer, 5, $len);
                $message = new BenchReply();
                $message->mergeFromString($payload);
                $buffer = substr($buffer, 5 + $len);
            }
        },
    ];

    $cases[] = [
        'name' => "stream split offset + merge $messageCount messages",
        'revs' => (int) max(1_000, $revs / 2),
        'subject' => static function () use ($streamBuffer): void {
            $bufferLength = strlen($streamBuffer);
            $offset = 0;
            while ($bufferLength - $offset >= 5) {
                $len = unpack('N', substr($streamBuffer, $offset + 1, 4))[1];
                if ($bufferLength - $offset < 5 + $len) {
                    break;
                }
                $payload = substr($streamBuffer, $offset + 5, $len);
                $message = new BenchReply();
                $message->mergeFromString($payload);
                $offset += 5 + $len;
            }
        },
    ];
}

printf("%-52s %12s %12s\n", 'case', 'revs', 'avg');
printf("%'-80s\n", '');

foreach ($cases as $case) {
    // Warm CPU caches and protobuf metadata before timing.
    for ($i = 0; $i < 1_000; $i++) {
        ($case['subject'])();
    }

    $start = hrtime(true);
    for ($i = 0; $i < $case['revs']; $i++) {
        ($case['subject'])();
    }
    $elapsedNs = hrtime(true) - $start;
    $avgNs = $elapsedNs / $case['revs'];

    printf("%-52s %12d %12s\n", $case['name'], $case['revs'], formatDuration($avgNs));
}

function formatBytes(int $bytes): string
{
    return match ($bytes) {
        0 => '0B',
        100 => '100B',
        1024 => '1KB',
        10 * 1024 => '10KB',
        100 * 1024 => '100KB',
        default => $bytes . 'B',
    };
}

function formatDuration(float $ns): string
{
    if ($ns >= 1_000_000) {
        return sprintf('%.3f ms', $ns / 1_000_000);
    }
    if ($ns >= 1_000) {
        return sprintf('%.3f μs', $ns / 1_000);
    }
    return sprintf('%.1f ns', $ns);
}
