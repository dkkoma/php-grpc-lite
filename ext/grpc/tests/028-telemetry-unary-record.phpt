--TEST--
grpc telemetry handler receives a unary RPC record
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50051]);
?>
--INI--
grpc_lite.telemetry_enabled=1
grpc_lite.telemetry_detail_level=phase
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';
grpc_lite_phpt_require_autoload();

use Grpc\Channel;
use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

$records = [];
grpc_lite_set_telemetry_handler(static function (array $record) use (&$records): void {
    $records[] = $record;
});

$opts = ['credentials' => ChannelCredentials::createInsecure()];
$channel = new Channel('test-server:50051', $opts);
$client = new GreeterClient('test-server:50051', $opts, $channel);
$call = $client->BenchUnary(new BenchRequest(['payload_bytes' => 1]), [
    'traceparent' => ['00-0123456789abcdef0123456789abcdef-0123456789abcdef-01'],
]);
[$response, $status] = $call->wait();

grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'unary status');
grpc_lite_phpt_assert_same(1, count($records), 'one telemetry record');
$record = $records[0];
grpc_lite_phpt_assert_same('unary', $record['kind'], 'record kind');
grpc_lite_phpt_assert_same('grpc', $record['rpc_system'], 'rpc system');
grpc_lite_phpt_assert_same('helloworld.Greeter', $record['rpc_service'], 'rpc service');
grpc_lite_phpt_assert_same('BenchUnary', $record['rpc_method'], 'rpc method');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $record['grpc_status_code'], 'grpc status');
grpc_lite_phpt_assert_same('phase', $record['detail_level'], 'detail level');
grpc_lite_phpt_assert_same('00-0123456789abcdef0123456789abcdef-0123456789abcdef-01', $record['traceparent'], 'traceparent copied');
grpc_lite_phpt_assert_true(isset($record['timings']['recv_loop_us']), 'recv loop timing');
grpc_lite_phpt_assert_true(isset($record['sizes']['request_bytes']), 'request size');
grpc_lite_phpt_assert_true(isset($record['http2']['stream_id']), 'http2 stream id');
grpc_lite_phpt_assert_true(isset($record['connection']['persistent_reused']), 'connection persistence');

$call = $client->BenchServerStream(new BenchRequest(['message_count' => 2, 'payload_bytes' => 1]), [
    'traceparent' => ['00-0123456789abcdef0123456789abcdef-1111111111111111-01'],
]);
$messages = [];
foreach ($call->responses() as $message) {
    $messages[] = $message;
}
grpc_lite_phpt_assert_same(2, count($messages), 'server streaming messages');
grpc_lite_phpt_assert_same(2, count($records), 'server streaming telemetry record');
$streamRecord = $records[1];
grpc_lite_phpt_assert_same('server_streaming', $streamRecord['kind'], 'stream record kind');
grpc_lite_phpt_assert_same('BenchServerStream', $streamRecord['rpc_method'], 'stream rpc method');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $streamRecord['grpc_status_code'], 'stream grpc status');
grpc_lite_phpt_assert_same(2, $streamRecord['message_count'], 'stream message count');
grpc_lite_phpt_assert_same('00-0123456789abcdef0123456789abcdef-1111111111111111-01', $streamRecord['traceparent'], 'stream traceparent copied');
grpc_lite_phpt_assert_true(isset($streamRecord['timings']['recv_loop_us']), 'stream recv loop timing');
grpc_lite_phpt_assert_true(isset($streamRecord['sizes']['response_body_bytes']), 'stream response size');

grpc_lite_set_telemetry_handler(static function (array $record): void {
    throw new RuntimeException('telemetry handler failure must not affect RPC');
});
$call = $client->BenchUnary(new BenchRequest(['payload_bytes' => 1]));
[$response, $status] = $call->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'handler failure does not affect RPC');

grpc_lite_set_telemetry_handler(null);

echo "OK\n";
?>
--EXPECT--
OK
