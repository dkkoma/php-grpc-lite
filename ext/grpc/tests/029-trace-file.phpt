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
putenv('GRPC_LITE_TRACE_WIRE_BYTES=1');

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

[$followupResponse, $followupStatus] = $client->SayHello($request)->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $followupStatus->code, 'follow-up unary status');

putenv('GRPC_LITE_TRACE_FILE');
putenv('GRPC_LITE_TRACE_WIRE_BYTES');

$lines = array_values(array_filter(explode("\n", trim((string) file_get_contents($traceFile)))));
unlink($traceFile);

$records = array_map(static function (string $line): array {
    $record = json_decode($line, true);
    grpc_lite_phpt_assert_true(is_array($record), 'trace line must be JSON object');
    return $record;
}, $lines);

$unaryEnd = null;
$streamingEnd = null;
$unaryPathHeader = null;
$unaryHeadersFrame = null;
$unaryDataFrame = null;
$settingsFrame = null;
$connectionWindowUpdateFrame = null;
$inboundSettingsFrame = null;
$outboundPingPayloads = [];
$inboundPingAckPayloads = [];
foreach ($records as $record) {
    if (($record['event'] ?? null) === 'rpc.end' && ($record['rpc_kind'] ?? null) === 'unary') {
        $unaryEnd = $record;
    }
    if (($record['event'] ?? null) === 'rpc.end' && ($record['rpc_kind'] ?? null) === 'server_streaming') {
        $streamingEnd = $record;
    }
    if (($record['event'] ?? null) === 'wire.request_header'
        && ($record['rpc_method'] ?? null) === '/helloworld.Greeter/SayHello'
        && ($record['name'] ?? null) === ':path') {
        $unaryPathHeader = $record;
    }
    if (($record['event'] ?? null) === 'wire.frame_out'
        && ($record['rpc_method'] ?? null) === '/helloworld.Greeter/SayHello'
        && ($record['frame_type'] ?? null) === 'HEADERS') {
        $unaryHeadersFrame = $record;
    }
    if (($record['event'] ?? null) === 'wire.frame_out'
        && ($record['rpc_method'] ?? null) === '/helloworld.Greeter/SayHello'
        && ($record['frame_type'] ?? null) === 'DATA') {
        $unaryDataFrame = $record;
    }
    if (($record['event'] ?? null) === 'wire.frame_out'
        && ($record['stream_id'] ?? null) === 0
        && ($record['frame_type'] ?? null) === 'SETTINGS'
        && ($record['flags'] ?? null) === 0) {
        $settingsFrame = $record;
    }
    if (($record['event'] ?? null) === 'wire.frame_out'
        && ($record['stream_id'] ?? null) === 0
        && ($record['frame_type'] ?? null) === 'WINDOW_UPDATE') {
        $connectionWindowUpdateFrame = $record;
    }
    if (($record['event'] ?? null) === 'wire.frame_in'
        && ($record['stream_id'] ?? null) === 0
        && ($record['frame_type'] ?? null) === 'SETTINGS'
        && ($record['flags'] ?? null) === 0) {
        $inboundSettingsFrame = $record;
    }
    if (($record['event'] ?? null) === 'wire.frame_out'
        && ($record['stream_id'] ?? null) === 0
        && ($record['frame_type'] ?? null) === 'PING'
        && ($record['flags'] ?? null) === 0
        && isset($record['payload_hex'])) {
        $outboundPingPayloads[] = $record['payload_hex'];
    }
    if (($record['event'] ?? null) === 'wire.frame_in'
        && ($record['stream_id'] ?? null) === 0
        && ($record['frame_type'] ?? null) === 'PING'
        && ($record['flags'] ?? null) === 1
        && isset($record['payload_hex'])) {
        $inboundPingAckPayloads[] = $record['payload_hex'];
    }
}

grpc_lite_phpt_assert_true(is_array($unaryEnd), 'unary rpc.end exists');
grpc_lite_phpt_assert_same('grpc-lite', $unaryEnd['transport_impl'] ?? null, 'unary trace transport');
grpc_lite_phpt_assert_same('/helloworld.Greeter/SayHello', $unaryEnd['rpc_method'] ?? null, 'unary trace method');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $unaryEnd['status_code'] ?? null, 'unary trace status');
grpc_lite_phpt_assert_true(($unaryEnd['elapsed_us'] ?? 0) > 0, 'unary trace elapsed');

grpc_lite_phpt_assert_true(is_array($streamingEnd), 'streaming rpc.end exists');
grpc_lite_phpt_assert_same('/helloworld.Greeter/SayManyHellos', $streamingEnd['rpc_method'] ?? null, 'streaming trace method');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $streamingEnd['status_code'] ?? null, 'streaming trace status');
grpc_lite_phpt_assert_true(array_key_exists('persistent_reused', $streamingEnd), 'streaming persistent reuse field exists');
grpc_lite_phpt_assert_same(null, $streamingEnd['persistent_reused'], 'streaming persistent reuse is unknown');

grpc_lite_phpt_assert_true(is_array($unaryPathHeader), 'unary request :path header trace exists');
grpc_lite_phpt_assert_same('/helloworld.Greeter/SayHello', $unaryPathHeader['value'] ?? null, 'unary request :path value');
grpc_lite_phpt_assert_true(is_array($unaryHeadersFrame), 'unary HEADERS frame trace exists');
grpc_lite_phpt_assert_true(($unaryHeadersFrame['header_block_len'] ?? 0) > 0, 'unary HEADERS block length');
grpc_lite_phpt_assert_true(is_array($unaryDataFrame), 'unary DATA frame trace exists');
grpc_lite_phpt_assert_true(($unaryDataFrame['frame_payload_len'] ?? 0) > 5, 'unary DATA frame length');
grpc_lite_phpt_assert_true(is_array($settingsFrame), 'outbound SETTINGS trace exists');
grpc_lite_phpt_assert_same(2, $settingsFrame['settings_count'] ?? null, 'outbound SETTINGS count');
grpc_lite_phpt_assert_same('ENABLE_PUSH', $settingsFrame['settings'][0]['name'] ?? null, 'outbound SETTINGS enable push');
grpc_lite_phpt_assert_same('INITIAL_WINDOW_SIZE', $settingsFrame['settings'][1]['name'] ?? null, 'outbound SETTINGS initial window');
grpc_lite_phpt_assert_true(!array_key_exists('rpc_method', $settingsFrame), 'connection-level SETTINGS must not be attributed to an RPC');
grpc_lite_phpt_assert_true(is_array($connectionWindowUpdateFrame), 'outbound connection WINDOW_UPDATE trace exists');
grpc_lite_phpt_assert_true(($connectionWindowUpdateFrame['window_size_increment'] ?? 0) > 0, 'outbound connection WINDOW_UPDATE increment');
grpc_lite_phpt_assert_true(!array_key_exists('rpc_method', $connectionWindowUpdateFrame), 'connection-level WINDOW_UPDATE must not be attributed to an RPC');
grpc_lite_phpt_assert_true(is_array($inboundSettingsFrame), 'inbound SETTINGS trace exists');
grpc_lite_phpt_assert_true(array_key_exists('settings', $inboundSettingsFrame), 'inbound SETTINGS entries exist');
grpc_lite_phpt_assert_true($outboundPingPayloads !== [], 'active BDP probe outbound PING exists');
grpc_lite_phpt_assert_true(array_intersect($outboundPingPayloads, $inboundPingAckPayloads) !== [], 'active BDP probe ACK payload matches outbound PING');

echo "OK\n";
?>
--EXPECT--
OK
