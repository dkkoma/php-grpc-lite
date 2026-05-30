--TEST--
grpc request metadata validation and filtering
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50051]);
?>
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';
grpc_lite_phpt_require_autoload();

use Grpc\Channel;
use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

$channel = new Channel('test-server:50051', [
    'credentials' => ChannelCredentials::createInsecure(),
]);
$client = new GreeterClient('test-server:50051', [
    'credentials' => ChannelCredentials::createInsecure(),
], $channel);

$serverSeen = static function (string $observedKey, array $metadata) use ($client): array {
    $call = $client->BenchUnary(new BenchRequest(), $metadata);
    [, $status] = $call->wait();
    grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'metadata control status');

    $responseMetadata = $call->getMetadata();
    grpc_lite_phpt_assert_same($observedKey, $responseMetadata['x-bench-seen-000-key-bin'][0] ?? null, 'observed metadata key');
    $count = (int) ($responseMetadata['x-bench-seen-000-count'][0] ?? 0);
    $values = [];
    for ($index = 0; $index < $count; $index++) {
        $valueKey = sprintf('x-bench-seen-000-value-%03d-bin', $index);
        $values[] = $responseMetadata[$valueKey][0] ?? null;
    }
    return $values;
};

$serverStreamSeen = static function (string $observedKey, array $metadata) use ($client): array {
    $request = new BenchRequest();
    $request->setMessageCount(1);
    $call = $client->BenchServerStream($request, $metadata);
    foreach ($call->responses() as $_reply) {
    }
    grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $call->getStatus()->code, 'server streaming metadata control status');

    $responseMetadata = $call->getMetadata();
    grpc_lite_phpt_assert_same($observedKey, $responseMetadata['x-bench-seen-000-key-bin'][0] ?? null, 'server streaming observed metadata key');
    $count = (int) ($responseMetadata['x-bench-seen-000-count'][0] ?? 0);
    $values = [];
    for ($index = 0; $index < $count; $index++) {
        $valueKey = sprintf('x-bench-seen-000-value-%03d-bin', $index);
        $values[] = $responseMetadata[$valueKey][0] ?? null;
    }
    return $values;
};

$seen = $serverSeen('user-agent', [
    'x-bench-observe-metadata-key' => ['user-agent'],
    'user-agent' => ['user-agent-override'],
]);
grpc_lite_phpt_assert_true(!in_array('user-agent-override', $seen, true), 'user-agent override must be filtered');

grpc_lite_phpt_assert_same([''], $serverSeen('x-bench-empty', [
    'x-bench-observe-metadata-key' => ['x-bench-empty'],
    'x-bench-empty' => [''],
]), 'empty ASCII metadata value');

grpc_lite_phpt_assert_same(['upper'], $serverSeen('x-bench-upper', [
    'x-bench-observe-metadata-key' => ['x-bench-upper'],
    'X-Bench-Upper' => ['upper'],
]), 'uppercase metadata key normalization');

grpc_lite_phpt_assert_same(["\x00\x01\xff"], $serverSeen('x-bench-raw-bin', [
    'x-bench-observe-metadata-key' => ['x-bench-raw-bin'],
    'x-bench-raw-bin' => ["\x00\x01\xff"],
]), 'raw binary metadata value');

$growValues = [];
for ($index = 0; $index < 24; $index++) {
    $growValues[] = sprintf('value-%02d', $index);
}
grpc_lite_phpt_assert_same($growValues, $serverSeen('x-bench-grow', [
    'x-bench-observe-metadata-key' => ['x-bench-grow'],
    'x-bench-grow' => $growValues,
]), 'unary metadata values across inline growth boundary');
grpc_lite_phpt_assert_same($growValues, $serverStreamSeen('x-bench-grow', [
    'x-bench-observe-metadata-key' => ['x-bench-grow'],
    'x-bench-grow' => $growValues,
]), 'server streaming metadata values across inline growth boundary');

$rawMetadata = [
    'x-bench-observe-metadata-key' => ['x-bench-mutation'],
    'x-bench-mutation' => ['before'],
];
$rawRequest = new BenchRequest();
$rawCall = new Grpc\Call($channel, '/helloworld.Greeter/BenchUnary', Grpc\Timeval::infFuture());
$rawCall->startBatch([
    Grpc\OP_SEND_INITIAL_METADATA => $rawMetadata,
    Grpc\OP_SEND_MESSAGE => ['message' => $rawRequest->serializeToString()],
    Grpc\OP_SEND_CLOSE_FROM_CLIENT => true,
]);
$rawMetadata['x-bench-mutation'][0] = 'after';
$rawEvent = $rawCall->startBatch([
    Grpc\OP_RECV_INITIAL_METADATA => true,
    Grpc\OP_RECV_MESSAGE => true,
    Grpc\OP_RECV_STATUS_ON_CLIENT => true,
]);
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $rawEvent->status->code, 'raw metadata mutation status');
grpc_lite_phpt_assert_same('before', $rawEvent->metadata['x-bench-seen-000-value-000-bin'][0] ?? null, 'metadata is isolated after startBatch');

$assertInvalidMetadata = static function (array $metadata) use ($client): void {
    grpc_lite_phpt_expect_throw(static function () use ($client, $metadata): void {
        $call = $client->BenchUnary(new BenchRequest(), $metadata);
        $call->wait();
    }, 'metadata');
};

$assertInvalidMetadata([':path' => ['/evil.Service/Method']]);
$assertInvalidMetadata(['x bench space' => ['space-key']]);
$assertInvalidMetadata(['grpc-foo' => ['reserved']]);
$assertInvalidMetadata(['grpc-timeout' => ['1S']]);
$assertInvalidMetadata(['x-bench-crlf' => ["line\r\nbreak"]]);
$assertInvalidMetadata(['x-bench-utf8' => ['utf8-あ']]);
$assertInvalidMetadata(['x-bench-non-string' => [123]]);

$tooManyValues = ['x-bench-many' => array_fill(0, 257, 'v')];
$assertInvalidMetadata($tooManyValues);

grpc_lite_phpt_expect_throw(static function () use ($client): void {
    $call = $client->BenchServerStream(new BenchRequest(), [
        ':path' => ['/evil.Service/Method'],
    ]);
    foreach ($call->responses() as $_reply) {
    }
}, 'metadata');

grpc_lite_phpt_expect_throw(static function () use ($client): void {
    $call = $client->BenchServerStream(new BenchRequest(), [
        'x-bench-non-string' => [123],
    ]);
    foreach ($call->responses() as $_reply) {
    }
}, 'metadata');

grpc_lite_phpt_expect_throw(static function () use ($client, $tooManyValues): void {
    $call = $client->BenchServerStream(new BenchRequest(), $tooManyValues);
    foreach ($call->responses() as $_reply) {
    }
}, 'metadata');

echo "OK\n";
?>
--EXPECT--
OK
