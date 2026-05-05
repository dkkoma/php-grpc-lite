<?php
declare(strict_types=1);

require __DIR__ . '/ResultContract.php';

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;
use PhpGrpcLite\Tools\Phase2\ResultContract;

$args = $argv;
array_shift($args);

$suite = 'metadata-compat';
$implementation = 'php-grpc-lite';
$transport = 'native';
$output = null;
$target = 'test-server:50051';
$autoload = 'vendor/autoload.php';

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
    } elseif ($arg === '--transport') {
        ++$argIndex;
    } elseif (str_starts_with($arg, '--transport=')) {
        continue;
    } else {
        usage("unexpected argument: $arg");
    }
}

if ($suite === '' || $implementation === '' || $target === '' || $autoload === '' || $output === null || $output === '') {
    usage('suite, implementation, target, autoload, and output are required');
}

requireAutoload($autoload);

$client = new GreeterClient($target, [
    'credentials' => ChannelCredentials::createInsecure(),
]);

$cases = [
    'duplicate_ascii_unary' => [
        'call_type' => 'unary',
        'metadata' => [
            'x-bench-echo-ascii' => ['first', 'second', 'third'],
            'x-bench-response-duplicate' => ['first', 'second', 'third'],
        ],
    ],
    'duplicate_binary_unary' => [
        'call_type' => 'unary',
        'metadata' => [
            'x-bench-echo-bin' => ["\x00first", "\x01second,comma", "\xffthird"],
            'x-bench-response-duplicate-bin' => ["\x00first", "\x01second,comma", "\xffthird"],
        ],
    ],
    'many_and_large_request_metadata_unary' => [
        'call_type' => 'unary',
        'metadata' => manyAndLargeMetadata(),
    ],
    'duplicate_ascii_server_streaming' => [
        'call_type' => 'server_streaming',
        'metadata' => [
            'x-bench-echo-ascii' => ['stream-first', 'stream-second'],
            'x-bench-response-duplicate' => ['stream-first', 'stream-second'],
        ],
    ],
];

$measurements = [];
foreach ($cases as $name => $case) {
    $startedNs = hrtime(true);
    $observation = observeCase($client, $case['call_type'], $case['metadata']);
    $elapsedNs = hrtime(true) - $startedNs;

    $measurements[] = ResultContract::measurement(
        $name,
        'metadata-compat',
        $case['call_type'] === 'unary' ? 'BenchUnary' : 'BenchServerStream',
        [
            'target' => $target,
            'transport' => $implementation === 'php-grpc-lite' ? $transport : 'ext-grpc',
            'request_metadata' => encodeBinary($case['metadata']),
            'status_code' => $observation['status']['code'],
            'status_details' => $observation['status']['details'],
            'response_count' => $observation['response_count'],
            'initial_metadata' => encodeBinary($observation['initial_metadata']),
            'trailing_metadata' => encodeBinary($observation['trailing_metadata']),
            'status_metadata' => encodeBinary($observation['status']['metadata']),
        ],
        [
            'wall_time_ns' => ['value' => $elapsedNs, 'unit' => 'ns'],
        ],
    );
}

$document = ResultContract::document($suite, $implementation, $measurements);
writeDocument($output, $document);

printf("%-42s %8s %10s %10s\n", 'scenario', 'status', 'initial', 'trailing');
printf("%'-76s\n", '');
foreach ($measurements as $measurement) {
    $params = (array) $measurement['attributes'];
    printf(
        "%-42s %8d %10d %10d\n",
        $measurement['name'],
        $params['status_code'],
        count($params['initial_metadata']),
        count($params['trailing_metadata']),
    );
}
echo "JSON: $output\n";

/** @return array<string, list<string>> */
function manyAndLargeMetadata(): array
{
    $metadata = [];
    for ($index = 0; $index < 12; $index++) {
        $metadata[sprintf('x-bench-extra-%02d', $index)] = ['extra'];
    }
    $metadata['x-bench-echo-ascii'] = [str_repeat('v', 600)];
    return $metadata;
}

/**
 * @param array<string, list<string>> $metadata
 * @return array{response_count: int, status: array{code: int, details: string, metadata: array<string, list<string>>}, initial_metadata: array<string, list<string>>, trailing_metadata: array<string, list<string>>}
 */
function observeCase(GreeterClient $client, string $callType, array $metadata): array
{
    if ($callType === 'server_streaming') {
        $request = new BenchRequest();
        $request->setMessageCount(2);
        $request->setPayloadBytes(10);
        $call = $client->BenchServerStream($request, $metadata);
        $responseCount = 0;
        foreach ($call->responses() as $_reply) {
            $responseCount++;
        }
        $status = $call->getStatus();

        return [
            'response_count' => $responseCount,
            'status' => statusToArray($status),
            'initial_metadata' => $call->getMetadata(),
            'trailing_metadata' => $call->getTrailingMetadata(),
        ];
    }

    $call = $client->BenchUnary(new BenchRequest(), $metadata);
    [$response, $status] = $call->wait();

    return [
        'response_count' => $response === null ? 0 : 1,
        'status' => statusToArray($status),
        'initial_metadata' => $call->getMetadata(),
        'trailing_metadata' => $call->getTrailingMetadata(),
    ];
}

/** @return array{code: int, details: string, metadata: array<string, list<string>>} */
function statusToArray(\stdClass $status): array
{
    return [
        'code' => (int) ($status->code ?? 0),
        'details' => (string) ($status->details ?? ''),
        'metadata' => is_array($status->metadata ?? null) ? $status->metadata : [],
    ];
}

function encodeBinary(mixed $value): mixed
{
    if (is_string($value)) {
        return [
            'base64' => base64_encode($value),
            'printable' => preg_match('/^[\x20-\x7e]*$/', $value) === 1 ? $value : null,
        ];
    }
    if (!is_array($value)) {
        return $value;
    }

    $encoded = [];
    foreach ($value as $key => $item) {
        $encoded[$key] = encodeBinary($item);
    }
    return $encoded;
}

function requireAutoload(string $autoload): void
{
    if (!is_file($autoload)) {
        throw new \RuntimeException("autoload not found: $autoload");
    }
    require $autoload;
}

function writeDocument(string $output, array $document): void
{
    $json = json_encode($document, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES);
    if ($json === false) {
        throw new \RuntimeException('failed to encode JSON: ' . json_last_error_msg());
    }
    if (file_put_contents($output, $json . "\n") === false) {
        throw new \RuntimeException("failed to write output: $output");
    }
}

function usage(string $message): never
{
    fwrite(STDERR, $message . "\n");
    exit(2);
}
