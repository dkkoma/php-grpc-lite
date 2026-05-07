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

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

$client = new GreeterClient('test-server:50051', [
    'credentials' => ChannelCredentials::createInsecure(),
]);

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

echo "OK\n";
?>
--EXPECT--
OK
