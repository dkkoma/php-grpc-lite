<?php
declare(strict_types=1);

require __DIR__ . '/ResultContract.php';
require __DIR__ . '/ResourceSampler.php';
require __DIR__ . '/UnaryBenchHelper.php';

use Helloworld\BenchRequest;
use PhpGrpcLite\Tools\Phase2\ResourceSampler;
use PhpGrpcLite\Tools\Phase2\ResultContract;
use PhpGrpcLite\Tools\Phase2\UnaryBenchHelper;

$suite = 'curl-multiplex-streaming';
$implementation = 'php-curl-multi';
$output = null;
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';
$streams = 1000;
$concurrency = 1;
$messageCount = 10;
$payloadBytes = 102400;
$multiplex = false;

$args = $argv;
array_shift($args);
for ($argIndex = 0; $argIndex < count($args); $argIndex++) {
    $arg = $args[$argIndex];
    if ($arg === '--suite') {
        $suite = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--suite=')) {
        $suite = substr($arg, strlen('--suite='));
    } elseif ($arg === '--implementation') {
        $implementation = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--implementation=')) {
        $implementation = substr($arg, strlen('--implementation='));
    } elseif ($arg === '--output') {
        $output = $args[++$argIndex] ?? null;
    } elseif (str_starts_with($arg, '--output=')) {
        $output = substr($arg, strlen('--output='));
    } elseif ($arg === '--target') {
        $target = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--target=')) {
        $target = substr($arg, strlen('--target='));
    } elseif ($arg === '--autoload') {
        $autoload = $args[++$argIndex] ?? '';
    } elseif (str_starts_with($arg, '--autoload=')) {
        $autoload = substr($arg, strlen('--autoload='));
    } elseif ($arg === '--streams') {
        $streams = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--streams=')) {
        $streams = (int) substr($arg, strlen('--streams='));
    } elseif ($arg === '--concurrency') {
        $concurrency = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--concurrency=')) {
        $concurrency = (int) substr($arg, strlen('--concurrency='));
    } elseif ($arg === '--message-count') {
        $messageCount = (int) ($args[++$argIndex] ?? 0);
    } elseif (str_starts_with($arg, '--message-count=')) {
        $messageCount = (int) substr($arg, strlen('--message-count='));
    } elseif ($arg === '--payload-bytes') {
        $payloadBytes = (int) ($args[++$argIndex] ?? -1);
    } elseif (str_starts_with($arg, '--payload-bytes=')) {
        $payloadBytes = (int) substr($arg, strlen('--payload-bytes='));
    } elseif ($arg === '--multiplex') {
        $multiplex = true;
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($output === null || $output === '' || $target === '' || $autoload === '' || $streams <= 0 || $concurrency <= 0 || $messageCount <= 0 || $payloadBytes < 0) {
    usage('output, target, autoload, streams, concurrency, message-count, and payload-bytes must be valid');
}

if (!is_file($autoload)) {
    throw new RuntimeException("autoload file not found: $autoload");
}
require $autoload;

$request = new BenchRequest();
$request->setMessageCount($messageCount);
$request->setPayloadBytes($payloadBytes);
$requestFrame = grpcFrame($request->serializeToString());
$url = sprintf('http://%s/helloworld.Greeter/BenchServerStream', $target);

$streamLatenciesNs = [];
$streamMessages = [];
$numConnects = [];
$localPorts = [];
$runningHandles = [];
$nextStream = 0;
$completed = 0;

$sample = ResourceSampler::measure(static function () use (
    $streams,
    $concurrency,
    $messageCount,
    $requestFrame,
    $url,
    $multiplex,
    &$streamLatenciesNs,
    &$streamMessages,
    &$numConnects,
    &$localPorts,
    &$runningHandles,
    &$nextStream,
    &$completed
): int {
    $mh = curl_multi_init();
    if ($multiplex && defined('CURLMOPT_PIPELINING') && defined('CURLPIPE_MULTIPLEX')) {
        curl_multi_setopt($mh, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
    }
    if ($multiplex && defined('CURLMOPT_MAX_HOST_CONNECTIONS')) {
        curl_multi_setopt($mh, CURLMOPT_MAX_HOST_CONNECTIONS, 1);
    }

    try {
        while ($nextStream < $streams && count($runningHandles) < $concurrency) {
            addHandle($mh, $runningHandles, $nextStream++, $url, $requestFrame);
        }

        do {
            do {
                $execResult = curl_multi_exec($mh, $running);
            } while ($execResult === CURLM_CALL_MULTI_PERFORM);
            if ($execResult !== CURLM_OK) {
                throw new RuntimeException('curl_multi_exec failed: ' . curl_multi_strerror($execResult));
            }

            while ($info = curl_multi_info_read($mh)) {
                $ch = $info['handle'];
                $key = spl_object_id($ch);
                $state = $runningHandles[$key] ?? null;
                if ($state === null) {
                    continue;
                }
                unset($runningHandles[$key]);

                $latencyNs = hrtime(true) - $state['started_ns'];
                $streamLatenciesNs[] = $latencyNs;
                $streamMessages[] = $state['messages'];
                $curlInfo = curl_getinfo($ch);
                $numConnects[] = (int) ($curlInfo['num_connects'] ?? 0);
                if (isset($curlInfo['local_port'])) {
                    $localPorts[] = (int) $curlInfo['local_port'];
                }

                if ($info['result'] !== CURLE_OK) {
                    throw new RuntimeException('curl error: ' . curl_strerror($info['result']));
                }
                $grpcStatus = (int) ($state['headers']['grpc-status'][0] ?? -1);
                if ($grpcStatus !== 0) {
                    throw new RuntimeException("unexpected grpc status: $grpcStatus");
                }
                if ($state['messages'] !== $messageCount) {
                    throw new RuntimeException("expected $messageCount messages, got {$state['messages']}");
                }

                curl_multi_remove_handle($mh, $ch);
                curl_close($ch);
                $completed++;

                while ($nextStream < $streams && count($runningHandles) < $concurrency) {
                    addHandle($mh, $runningHandles, $nextStream++, $url, $requestFrame);
                }
            }

            if ($running > 0) {
                curl_multi_select($mh, 1.0);
            }
        } while ($completed < $streams);
    } finally {
        foreach ($runningHandles as $state) {
            curl_multi_remove_handle($mh, $state['handle']);
            curl_close($state['handle']);
        }
        curl_multi_close($mh);
    }

    return array_sum($streamMessages);
});

$messages = $sample['result'];
$metrics = $sample['metrics'];
$elapsedSec = $metrics['wall_time_ns_total']['value'] / 1_000_000_000;
$metrics['streams_total'] = ['value' => $streams, 'unit' => 'streams'];
$metrics['concurrency'] = ['value' => $concurrency, 'unit' => 'streams'];
$metrics['messages_total'] = ['value' => $messages, 'unit' => 'messages'];
$metrics['messages_per_second'] = ['value' => $messages / $elapsedSec, 'unit' => 'messages/s'];
$metrics['streams_per_second'] = ['value' => $streams / $elapsedSec, 'unit' => 'streams/s'];
$metrics['curl_num_connects_total'] = ['value' => array_sum($numConnects), 'unit' => 'connections'];
$metrics['curl_local_ports_unique'] = ['value' => count(array_unique($localPorts)), 'unit' => 'ports'];
foreach (UnaryBenchHelper::percentiles($streamLatenciesNs) as $name => $value) {
    $metrics['stream_latency_' . $name . '_ns'] = ['value' => $value, 'unit' => 'ns'];
}

$measurement = ResultContract::measurement('curl_multiplex_streaming', $suite, 'BenchServerStream', [
    'target' => $target,
    'streams' => $streams,
    'concurrency' => $concurrency,
    'message_count' => $messageCount,
    'payload_bytes' => $payloadBytes,
    'multiplex' => $multiplex,
], $metrics);

$document = ResultContract::document($suite, $implementation, [$measurement]);
$dir = dirname($output);
if (!is_dir($dir)) {
    mkdir($dir, 0777, true);
}
file_put_contents($output, ResultContract::encode($document));

printf("%-28s %8s %12s %12s %12s %12s %8s %8s\n", 'scenario', 'conc', 'messages', 'msg/s', 'p50 stream', 'p99 stream', 'conn', 'ports');
printf("%'-104s\n", '');
printf(
    "%-28s %8d %12d %12.1f %11.1fμs %11.1fμs %8d %8d\n",
    $measurement['name'],
    $concurrency,
    $messages,
    $metrics['messages_per_second']['value'],
    $metrics['stream_latency_p50_ns']['value'] / 1_000,
    $metrics['stream_latency_p99_ns']['value'] / 1_000,
    $metrics['curl_num_connects_total']['value'],
    $metrics['curl_local_ports_unique']['value'],
);
echo "JSON: $output\n";

function addHandle(CurlMultiHandle $mh, array &$runningHandles, int $streamIndex, string $url, string $requestFrame): void
{
    $state = [
        'index' => $streamIndex,
        'started_ns' => hrtime(true),
        'buffer' => '',
        'messages' => 0,
        'headers' => [],
        'handle' => null,
    ];

    $ch = curl_init();
    $state['handle'] = $ch;
    curl_setopt_array($ch, [
        CURLOPT_URL => $url,
        CURLOPT_HTTP_VERSION => CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE,
        CURLOPT_POST => true,
        CURLOPT_POSTFIELDS => $requestFrame,
        CURLOPT_HTTPHEADER => [
            'Content-Type: application/grpc',
            'TE: trailers',
            'User-Agent: php-grpc-lite-phase2-curl-multiplex/0.0.1',
            'x-bench-server-cached-payload: 1',
        ],
        CURLOPT_RETURNTRANSFER => false,
        CURLOPT_HEADERFUNCTION => static function ($ch, string $line) use (&$state): int {
            $trimmed = rtrim($line, "\r\n");
            if ($trimmed !== '' && !str_starts_with($trimmed, 'HTTP/')) {
                [$key, $value] = array_pad(explode(':', $trimmed, 2), 2, '');
                $state['headers'][strtolower(trim($key))][] = ltrim($value);
            }

            return strlen($line);
        },
        CURLOPT_WRITEFUNCTION => static function ($ch, string $chunk) use (&$state): int {
            $state['buffer'] .= $chunk;
            while (strlen($state['buffer']) >= 5) {
                $payloadLength = unpack('N', substr($state['buffer'], 1, 4))[1];
                if (strlen($state['buffer']) < 5 + $payloadLength) {
                    break;
                }
                $state['messages']++;
                $state['buffer'] = substr($state['buffer'], 5 + $payloadLength);
            }

            return strlen($chunk);
        },
    ]);

    $runningHandles[spl_object_id($ch)] = &$state;
    curl_multi_add_handle($mh, $ch);
}

function grpcFrame(string $payload): string
{
    return "\0" . pack('N', strlen($payload)) . $payload;
}

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n\n");
    fwrite(STDERR, "Usage: php tools/phase2/curl-multiplex-streaming.php --output=var/bench-results/result.json [--streams=1000] [--concurrency=8] [--message-count=10] [--payload-bytes=102400] [--multiplex]\n");
    exit(2);
}
