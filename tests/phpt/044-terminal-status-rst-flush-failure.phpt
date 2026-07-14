--TEST--
terminal status-field failure keeps status details primary when CANCEL flush fails
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50071]);
grpc_lite_phpt_skip_if_test_fault_seam_unavailable();
?>
--ENV--
GRPC_LITE_TRACE_FILE=/tmp/grpc-lite-trace-044.jsonl
GRPC_LITE_TEST_FAULT=terminal-status-rst-flush-fatal
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';
grpc_lite_phpt_require_autoload();

use Grpc\Channel;
use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

$traceFile = (string) getenv('GRPC_LITE_TRACE_FILE');
file_put_contents($traceFile, '');

$clientSequence = 0;
$client = static function () use (&$clientSequence): GreeterClient {
    $clientSequence++;
    $opts = [
        'credentials' => ChannelCredentials::createInsecure(),
        'grpc.default_authority' => "terminal-flush-fault-$clientSequence.test",
    ];
    $channel = new Channel('test-server:50071', $opts);
    return new GreeterClient('test-server:50071', $opts, $channel);
};

$assertHealthyFollowUp = static function (GreeterClient $client, string $label): void {
    [$response, $status] = $client->BenchUnary(new BenchRequest(), [
        'x-bench-raw-response' => ['valid-after-terminal-flush-fault'],
    ])->wait();
    grpc_lite_phpt_assert_true($response instanceof \Helloworld\BenchReply, "$label follow-up response");
    grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $status->code, "$label follow-up status");
};

foreach ([
    'post-informational-silent-grpc-status' => 'grpc-status',
    'post-informational-silent-grpc-message' => 'grpc-message',
    'post-informational-silent-status-details' => 'grpc-status-details-bin',
] as $control => $label) {
    $unaryClient = $client();
    [$response, $status] = $unaryClient->BenchUnary(new BenchRequest(), [
        'x-bench-raw-response' => [$control],
    ], ['timeout' => 2_000_000])->wait();
    grpc_lite_phpt_assert_same(null, $response, "$label unary response");
    grpc_lite_phpt_assert_same(Grpc\STATUS_UNKNOWN, $status->code, "$label unary status");
    grpc_lite_phpt_assert_same('invalid grpc-status trailer', $status->details, "$label unary details");
    $assertHealthyFollowUp($unaryClient, "$label unary");

    $streamClient = $client();
    $request = new BenchRequest();
    $request->setMessageCount(1);
    $call = $streamClient->BenchServerStream($request, [
        'x-bench-raw-response' => [$control],
    ], ['timeout' => 2_000_000]);
    $count = 0;
    foreach ($call->responses() as $_reply) {
        $count++;
    }
    grpc_lite_phpt_assert_same(0, $count, "$label stream count");
    grpc_lite_phpt_assert_same(Grpc\STATUS_UNKNOWN, $call->getStatus()->code, "$label stream status");
    grpc_lite_phpt_assert_same('invalid grpc-status trailer', $call->getStatus()->details, "$label stream details");
    $assertHealthyFollowUp($streamClient, "$label stream");
}

$prefaceCount = 0;
foreach (file($traceFile, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
    $record = json_decode($line, true);
    grpc_lite_phpt_assert_true(is_array($record), 'trace line must be JSON object');
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $prefaceCount++;
    }
}
unlink($traceFile);

// Each isolated client opens one connection for the failing call and a fresh
// one for its follow-up. A failed CANCEL flush must evict the dead connection.
grpc_lite_phpt_assert_same(12, $prefaceCount, 'CANCEL flush failures evict every connection');

echo "OK\n";
?>
--EXPECT--
OK
