--TEST--
grpc transport control semantics handle malformed frames, RST_STREAM, GOAWAY, and abandoned streams
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50051, 50055, 50056, 50057, 50058, 50059, 50060]);
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

// Scenario group 1: malformed response frame and connection setup failure.
[, $status] = $client('test-server:50057')->BenchUnary(new BenchRequest())->wait();
grpc_lite_phpt_assert_true($status->code !== Grpc\STATUS_OK, 'malformed unary frame must fail');

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
grpc_lite_phpt_assert_same(null, $firstResponse, 'RST unary first response');
grpc_lite_phpt_assert_same(Grpc\STATUS_UNAVAILABLE, $firstStatus->code, 'RST unary first status');
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
grpc_lite_phpt_assert_same(0, $firstCount, 'RST stream first count');
grpc_lite_phpt_assert_same(Grpc\STATUS_UNAVAILABLE, $firstCall->getStatus()->code, 'RST stream first status');
grpc_lite_phpt_assert_same(1, $secondCount, 'RST stream second count');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $secondCall->getStatus()->code, 'RST stream second status');

[$goAwayResponse, $goAwayStatus] = $client('test-server:50060')->BenchUnary(new BenchRequest())->wait();
grpc_lite_phpt_assert_same(null, $goAwayResponse, 'GOAWAY response');
grpc_lite_phpt_assert_same(Grpc\STATUS_UNAVAILABLE, $goAwayStatus->code, 'GOAWAY status');
grpc_lite_phpt_assert_same('HTTP/2 stream refused by GOAWAY', $goAwayStatus->details, 'GOAWAY details');

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
