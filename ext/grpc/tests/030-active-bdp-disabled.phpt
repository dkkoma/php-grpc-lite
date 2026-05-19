--TEST--
grpc-lite active BDP probe is disabled by default and does not emit probe PINGs
--INI--
grpc_lite.http2_stream_window_size=1048576
grpc_lite.active_bdp_probe=0
grpc_lite.active_bdp_update_settings=1
grpc_lite.active_bdp_update_max_frame_size=1
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

$traceFile = tempnam(sys_get_temp_dir(), 'grpc-lite-bdp-disabled-');
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

$stream = $client->SayManyHellos($request);
foreach ($stream->responses() as $_reply) {
}
$status = $stream->getStatus();
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'streaming status');

putenv('GRPC_LITE_TRACE_FILE');
putenv('GRPC_LITE_TRACE_WIRE_BYTES');

$lines = array_values(array_filter(explode("\n", trim((string) file_get_contents($traceFile)))));
unlink($traceFile);

$clientOriginPings = 0;
$bdpSettingsFrames = 0;
foreach ($lines as $line) {
    $record = json_decode($line, true);
    grpc_lite_phpt_assert_true(is_array($record), 'trace line must be JSON object');
    if (($record['event'] ?? null) === 'wire.frame_out'
        && ($record['stream_id'] ?? null) === 0
        && ($record['frame_type'] ?? null) === 'PING'
        && ($record['flags'] ?? null) === 0) {
        $clientOriginPings++;
    }
    if (($record['event'] ?? null) === 'wire.frame_out'
        && ($record['stream_id'] ?? null) === 0
        && ($record['frame_type'] ?? null) === 'SETTINGS'
        && ($record['flags'] ?? null) === 0) {
        $hasBdpInitialWindow = false;
        $hasMaxFrame = false;
        foreach ($record['settings'] ?? [] as $setting) {
            if (($setting['name'] ?? null) === 'INITIAL_WINDOW_SIZE' && ($setting['value'] ?? null) === 8388608) {
                $hasBdpInitialWindow = true;
            }
            if (($setting['name'] ?? null) === 'MAX_FRAME_SIZE') {
                $hasMaxFrame = true;
            }
        }
        if ($hasBdpInitialWindow || $hasMaxFrame) {
            $bdpSettingsFrames++;
        }
    }
}

grpc_lite_phpt_assert_same(0, $clientOriginPings, 'disabled active BDP probe does not emit client-origin PING');
grpc_lite_phpt_assert_same(0, $bdpSettingsFrames, 'disabled active BDP probe prevents ACK-triggered SETTINGS update');

echo "OK\n";
?>
--EXPECT--
OK
