--TEST--
grpc-lite trace file records unary and server streaming RPC completion
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
use Helloworld\HelloRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

$traceFile = tempnam(sys_get_temp_dir(), 'grpc-lite-trace-');
if ($traceFile === false) {
    throw new RuntimeException('failed to create trace file');
}
file_put_contents($traceFile, '');
putenv('GRPC_LITE_TRACE_FILE=' . $traceFile);

$opts = ['credentials' => ChannelCredentials::createInsecure()];
$channel = new Channel('test-server:50051', $opts);
$client = new GreeterClient('test-server:50051', $opts, $channel);

$request = new HelloRequest();
$request->setName('Trace');

[$response, $status] = $client->SayHello($request)->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'unary status');

$stream = $client->SayManyHellos($request);
$streamStatus = null;
foreach ($stream->responses() as $_reply) {
}
$streamStatus = $stream->getStatus();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $streamStatus->code, 'streaming status');

putenv('GRPC_LITE_TRACE_FILE');

$lines = array_values(array_filter(explode("\n", trim((string) file_get_contents($traceFile)))));
unlink($traceFile);
grpc_lite_phpt_assert_same(2, count($lines), 'trace line count');

$records = array_map(static function (string $line): array {
    $record = json_decode($line, true);
    grpc_lite_phpt_assert_true(is_array($record), 'trace line must be JSON object');
    return $record;
}, $lines);

grpc_lite_phpt_assert_same('rpc.end', $records[0]['event'] ?? null, 'unary trace event');
grpc_lite_phpt_assert_same('grpc-lite', $records[0]['transport_impl'] ?? null, 'unary trace transport');
grpc_lite_phpt_assert_same('unary', $records[0]['rpc_kind'] ?? null, 'unary trace kind');
grpc_lite_phpt_assert_same('/helloworld.Greeter/SayHello', $records[0]['rpc_method'] ?? null, 'unary trace method');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $records[0]['status_code'] ?? null, 'unary trace status');
grpc_lite_phpt_assert_true(($records[0]['elapsed_us'] ?? 0) > 0, 'unary trace elapsed');

grpc_lite_phpt_assert_same('rpc.end', $records[1]['event'] ?? null, 'streaming trace event');
grpc_lite_phpt_assert_same('server_streaming', $records[1]['rpc_kind'] ?? null, 'streaming trace kind');
grpc_lite_phpt_assert_same('/helloworld.Greeter/SayManyHellos', $records[1]['rpc_method'] ?? null, 'streaming trace method');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $records[1]['status_code'] ?? null, 'streaming trace status');
grpc_lite_phpt_assert_true(array_key_exists('persistent_reused', $records[1]), 'streaming persistent reuse field exists');
grpc_lite_phpt_assert_same(null, $records[1]['persistent_reused'], 'streaming persistent reuse is unknown');

echo "OK\n";
?>
--EXPECT--
OK
