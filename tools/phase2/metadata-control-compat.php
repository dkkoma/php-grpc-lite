<?php
declare(strict_types=1);

require __DIR__ . '/ResultContract.php';

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;
use PhpGrpcLite\Tools\Phase2\ResultContract;

$args = $argv;
array_shift($args);

$suite = 'metadata-control-compat';
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
    'reserved_grpc_status' => metadataCase('grpc-status', '7'),
    'reserved_grpc_message' => metadataCase('grpc-message', 'user message'),
    'reserved_grpc_timeout' => metadataCase('grpc-timeout', '1S'),
    'reserved_grpc_encoding' => metadataCase('grpc-encoding', 'gzip'),
    'fixed_te' => metadataCase('te', 'not-trailers'),
    'fixed_content_type' => metadataCase('content-type', 'application/not-grpc'),
    'fixed_user_agent' => metadataCase('user-agent', 'user-agent-override'),
    'pseudo_authority' => metadataCase(':authority', 'evil.example.test'),
    'pseudo_path' => metadataCase(':path', '/evil.Service/Method'),
    'uppercase_key' => metadataCase('X-Bench-Upper', 'upper'),
    'underscore_key' => metadataCase('x_bench_under', 'under'),
    'dot_key' => metadataCase('x.bench.dot', 'dot'),
    'space_key' => metadataCase('x bench space', 'space-key'),
    'utf8_key' => metadataCase('x-bench-ümlaut', 'umlaut-key'),
    'empty_value' => metadataCase('x-bench-empty', ''),
    'leading_space_value' => metadataCase('x-bench-leading-space', ' leading'),
    'trailing_space_value' => metadataCase('x-bench-trailing-space', 'trailing '),
    'comma_value' => metadataCase('x-bench-comma', 'a,b'),
    'crlf_value' => metadataCase('x-bench-crlf', "line\r\nbreak"),
    'utf8_value' => metadataCase('x-bench-utf8', 'utf8-あ'),
];

$measurements = [];
foreach ($cases as $name => $case) {
    $startedNs = hrtime(true);
    $observation = observeCase($client, $case['metadata']);
    $elapsedNs = hrtime(true) - $startedNs;

    $measurements[] = ResultContract::measurement(
        $name,
        'metadata-control-compat',
        'BenchUnary',
        [
            'target' => $target,
            'transport' => $implementation === 'php-grpc-lite' ? $transport : 'ext-grpc',
            'observed_key' => $case['observed_key'],
            'request_metadata' => encodeBinary($case['metadata']),
            'exception_class' => $observation['exception_class'],
            'exception_message' => $observation['exception_message'],
            'status_code' => $observation['status']['code'],
            'status_details' => $observation['status']['details'],
            'server_seen' => encodeBinary($observation['server_seen']),
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

printf("%-30s %8s %-18s %-30s\n", 'scenario', 'status', 'seen', 'exception');
printf("%'-90s\n", '');
foreach ($measurements as $measurement) {
    $attributes = (array) $measurement['attributes'];
    $seen = (array) $attributes['server_seen'];
    printf(
        "%-30s %8d %-18s %-30s\n",
        $measurement['name'],
        $attributes['status_code'],
        seenSummary($seen),
        $attributes['exception_class'] ?? '',
    );
}
echo "JSON: $output\n";

/** @return array{observed_key: string, metadata: array<string, list<string>>} */
function metadataCase(string $key, string $value): array
{
    return [
        'observed_key' => $key,
        'metadata' => [
            'x-bench-observe-metadata-key' => [$key],
            $key => [$value],
        ],
    ];
}

/**
 * @param array<string, list<string>> $metadata
 * @return array{exception_class: string|null, exception_message: string|null, status: array{code: int, details: string, metadata: array<string, list<string>>}, server_seen: array<string, mixed>, initial_metadata: array<string, list<string>>, trailing_metadata: array<string, list<string>>}
 */
function observeCase(GreeterClient $client, array $metadata): array
{
    try {
        $call = $client->BenchUnary(new BenchRequest(), $metadata);
        [, $status] = $call->wait();

        return [
            'exception_class' => null,
            'exception_message' => null,
            'status' => statusToArray($status),
            'server_seen' => extractServerSeen($call->getMetadata()),
            'initial_metadata' => $call->getMetadata(),
            'trailing_metadata' => $call->getTrailingMetadata(),
        ];
    } catch (\Throwable $e) {
        return [
            'exception_class' => $e::class,
            'exception_message' => $e->getMessage(),
            'status' => ['code' => -1, 'details' => '', 'metadata' => []],
            'server_seen' => [],
            'initial_metadata' => [],
            'trailing_metadata' => [],
        ];
    }
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

/**
 * @param array<string, list<string>> $metadata
 * @return array<string, mixed>
 */
function extractServerSeen(array $metadata): array
{
    $seen = [];
    for ($index = 0; $index < 100; $index++) {
        $prefix = sprintf('x-bench-seen-%03d', $index);
        $keyValues = $metadata[$prefix . '-key-bin'] ?? null;
        if ($keyValues === null || $keyValues === []) {
            break;
        }
        $key = $keyValues[0];
        $count = (int) ($metadata[$prefix . '-count'][0] ?? 0);
        $values = [];
        for ($valueIndex = 0; $valueIndex < $count; $valueIndex++) {
            $valueKey = sprintf('%s-value-%03d-bin', $prefix, $valueIndex);
            if (!isset($metadata[$valueKey][0])) {
                continue;
            }
            $values[] = $metadata[$valueKey][0];
        }
        $seen[$key] = $values;
    }

    return $seen;
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

/** @param array<string, mixed> $seen */
function seenSummary(array $seen): string
{
    if ($seen === []) {
        return '-';
    }
    $key = (string) array_key_first($seen);
    $values = is_array($seen[$key] ?? null) ? $seen[$key] : [];
    return $key . '=' . count($values);
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
