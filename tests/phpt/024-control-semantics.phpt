--TEST--
grpc transport control semantics handle malformed frames, RST_STREAM, GOAWAY, and abandoned streams
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50051, 50055, 50056, 50057, 50058, 50059, 50060, 50061, 50062, 50063, 50064, 50065]);
?>
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';
grpc_lite_phpt_require_autoload();

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

$client = static function (string $target = 'test-server:50051'): GreeterClient {
    return new GreeterClient($target, [
        'credentials' => ChannelCredentials::createInsecure(),
    ]);
};

// Scenario group 1: connection break mid-message and connection setup failure.
// :50057 sends response HEADERS plus a partial gRPC message header, then
// closes the TCP connection: a client-observed connection break maps to
// UNAVAILABLE for both call kinds (INTERNAL is reserved for a clean
// END_STREAM inside a Length-Prefixed-Message).
[, $status] = $client('test-server:50057')->BenchUnary(new BenchRequest())->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_UNAVAILABLE, $status->code, 'unary connection break mid-message status');

$midStreamCall = $client('test-server:50057')->BenchServerStream(new BenchRequest());
foreach ($midStreamCall->responses() as $_reply) {
}
grpc_lite_phpt_assert_same(Grpc\STATUS_UNAVAILABLE, $midStreamCall->getStatus()->code, 'streaming connection break mid-message status');

[, $status] = $client('test-server:59999')->BenchUnary(new BenchRequest(), [], ['timeout' => 50000])->wait();
grpc_lite_phpt_assert_same(Grpc\STATUS_UNAVAILABLE, $status->code, 'connection refused unary status');

$refusedStreamCall = $client('test-server:59999')->BenchServerStream(new BenchRequest(), [], ['timeout' => 50000]);
$refusedStreamCount = 0;
foreach ($refusedStreamCall->responses() as $_reply) {
    $refusedStreamCount++;
}
grpc_lite_phpt_assert_same(0, $refusedStreamCount, 'connection refused stream count');
grpc_lite_phpt_assert_same(Grpc\STATUS_UNAVAILABLE, $refusedStreamCall->getStatus()->code, 'connection refused stream status');

// Scenario group 2: GOAWAY / EOF lifecycle and recovery.
$goAwayAfterOk = $client('test-server:50055');
[$response, $status] = $goAwayAfterOk->BenchUnary(new BenchRequest())->wait();
grpc_lite_phpt_assert_true($response !== null, 'GOAWAY after OK response');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'GOAWAY after OK status');

$firstEof = $client('test-server:50056');
$eofStatuses = [];
$eofResponses = [];
for ($attempt = 0; $attempt < 3; $attempt++) {
    [$response, $status] = $firstEof->BenchUnary(new BenchRequest())->wait();
    $eofResponses[] = $response;
    $eofStatuses[] = $status->code;
}
grpc_lite_phpt_assert_true(in_array(Grpc\STATUS_UNAVAILABLE, $eofStatuses, true), 'EOF sequence includes unavailable status');
grpc_lite_phpt_assert_true(in_array(Grpc\STATUS_OK, $eofStatuses, true), 'EOF sequence includes recovery OK status');
foreach ($eofStatuses as $index => $code) {
    if ($code === Grpc\STATUS_OK) {
        grpc_lite_phpt_assert_true($eofResponses[$index] !== null, 'EOF recovery response');
    }
}

// Scenario group 3: server-sent RST_STREAM must be stream-local and recoverable.
$rstUnary = $client('test-server:50058');
[$firstResponse, $firstStatus] = $rstUnary->BenchUnary(new BenchRequest())->wait();
[$secondResponse, $secondStatus] = $rstUnary->BenchUnary(new BenchRequest())->wait();
grpc_lite_phpt_assert_true($firstResponse !== null, 'RST unary first response');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $firstStatus->code, 'RST unary first status');
grpc_lite_phpt_assert_true($secondResponse !== null, 'RST unary second response');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $secondStatus->code, 'RST unary second status');

$rstStream = $client('test-server:50059');
$firstCall = $rstStream->BenchServerStream(new BenchRequest());
$firstCount = 0;
foreach ($firstCall->responses() as $_reply) {
    $firstCount++;
}
$secondCall = $rstStream->BenchServerStream(new BenchRequest());
$secondCount = 0;
foreach ($secondCall->responses() as $_reply) {
    $secondCount++;
}
grpc_lite_phpt_assert_same(1, $firstCount, 'RST stream first count');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $firstCall->getStatus()->code, 'RST stream first status');
grpc_lite_phpt_assert_same(1, $secondCount, 'RST stream second count');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $secondCall->getStatus()->code, 'RST stream second status');

[$goAwayRetryResponse, $goAwayRetryStatus] = $client('test-server:50061')->BenchUnary(new BenchRequest())->wait();
grpc_lite_phpt_assert_true($goAwayRetryResponse !== null, 'GOAWAY transparent retry unary response');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $goAwayRetryStatus->code, 'GOAWAY transparent retry unary status');

$goAwayRetryRequest = new BenchRequest();
$goAwayRetryRequest->setMessageCount(1);
$goAwayRetryCall = $client('test-server:50063')->BenchServerStream($goAwayRetryRequest);
$goAwayRetryCount = 0;
foreach ($goAwayRetryCall->responses() as $_reply) {
    $goAwayRetryCount++;
}
grpc_lite_phpt_assert_same(1, $goAwayRetryCount, 'GOAWAY transparent retry stream count');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $goAwayRetryCall->getStatus()->code, 'GOAWAY transparent retry stream status');

[$goAwayMaxResponse, $goAwayMaxStatus] = $client('test-server:50062')->BenchUnary(new BenchRequest())->wait();
grpc_lite_phpt_assert_true($goAwayMaxResponse !== null, 'GOAWAY MaxInt32 unary response');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $goAwayMaxStatus->code, 'GOAWAY MaxInt32 unary status');

[$goAwayResponse, $goAwayStatus] = $client('test-server:50060')->BenchUnary(new BenchRequest())->wait();
grpc_lite_phpt_assert_same(null, $goAwayResponse, 'GOAWAY response');
grpc_lite_phpt_assert_same(Grpc\STATUS_UNAVAILABLE, $goAwayStatus->code, 'GOAWAY status');
grpc_lite_phpt_assert_same('HTTP/2 stream refused by GOAWAY', $goAwayStatus->details, 'GOAWAY details');

$goAwayAlwaysRefusedCall = $client('test-server:50060')->BenchServerStream(new BenchRequest());
$goAwayAlwaysRefusedCount = 0;
foreach ($goAwayAlwaysRefusedCall->responses() as $_reply) {
    $goAwayAlwaysRefusedCount++;
}
grpc_lite_phpt_assert_same(0, $goAwayAlwaysRefusedCount, 'GOAWAY always refused stream count');
grpc_lite_phpt_assert_same(Grpc\STATUS_UNAVAILABLE, $goAwayAlwaysRefusedCall->getStatus()->code, 'GOAWAY always refused stream status');

$goAwayAfterMessageCall = $client('test-server:50064')->BenchServerStream(new BenchRequest());
$goAwayAfterMessageCount = 0;
foreach ($goAwayAfterMessageCall->responses() as $_reply) {
    $goAwayAfterMessageCount++;
}
grpc_lite_phpt_assert_same(1, $goAwayAfterMessageCount, 'GOAWAY after message stream count');
grpc_lite_phpt_assert_same(Grpc\STATUS_UNAVAILABLE, $goAwayAfterMessageCall->getStatus()->code, 'GOAWAY after message stream status');

$deadlineRetryCall = $client('test-server:50065')->BenchServerStream(new BenchRequest(), [], ['timeout' => 50000]);
$deadlineRetryCount = 0;
foreach ($deadlineRetryCall->responses() as $_reply) {
    $deadlineRetryCount++;
}
grpc_lite_phpt_assert_same(0, $deadlineRetryCount, 'GOAWAY retry deadline stream count');
grpc_lite_phpt_assert_same(Grpc\STATUS_DEADLINE_EXCEEDED, $deadlineRetryCall->getStatus()->code, 'GOAWAY retry deadline stream status');

// Scenario group 4: abandoning a server stream must not poison later RPCs.
$mainClient = $client();
$streamRequest = new BenchRequest();
$streamRequest->setMessageCount(3);
$streamRequest->setPayloadBytes(10);
$streamRequest->setServerDelayMs(10);
$streamCall = $mainClient->BenchServerStream($streamRequest);
foreach ($streamCall->responses() as $_reply) {
    break;
}
[$response, $status] = $mainClient->BenchUnary(new BenchRequest())->wait();
grpc_lite_phpt_assert_true($response !== null, 'unary after abandoned stream response');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, 'unary after abandoned stream status');
$streamCall->cancel();
$streamCall->cancel();

echo "OK\n";
?>
--EXPECT--
OK
