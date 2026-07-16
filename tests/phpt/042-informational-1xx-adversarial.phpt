--TEST--
informational response phases reject malformed wire sequences and enforce wire header budgets
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50071]);
?>
--ENV--
GRPC_LITE_TRACE_FILE=/tmp/grpc-lite-trace-042.jsonl
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';
grpc_lite_phpt_require_autoload();

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

$traceFile = (string) getenv('GRPC_LITE_TRACE_FILE');
file_put_contents($traceFile, '');

$clientSequence = 0;
$client = static function (array $options = []) use (&$clientSequence): GreeterClient {
    $clientSequence++;
    return new GreeterClient('test-server:50071', $options + [
        'credentials' => ChannelCredentials::createInsecure(),
        'grpc.default_authority' => "informational-probe-$clientSequence.test",
    ]);
};

$assertMalformedUnary = static function (GreeterClient $client, string $control, string $label): void {
    [$response, $status] = $client->BenchUnary(new BenchRequest(), [
        'x-bench-raw-response' => [$control],
    ])->wait();
    grpc_lite_phpt_assert_same(null, $response, "$label unary response");
    grpc_lite_phpt_assert_same(Grpc\STATUS_INTERNAL, $status->code, "$label unary status");
    grpc_lite_phpt_assert_same('malformed HTTP/2 response header sequence', $status->details, "$label unary details");
};

$assertMalformedStream = static function (GreeterClient $client, string $control, string $label, int $expectedCount): void {
    $request = new BenchRequest();
    $request->setMessageCount(1);
    $call = $client->BenchServerStream($request, [
        'x-bench-raw-response' => [$control],
    ]);
    $count = 0;
    foreach ($call->responses() as $_reply) {
        $count++;
    }
    grpc_lite_phpt_assert_same($expectedCount, $count, "$label stream count");
    grpc_lite_phpt_assert_same(Grpc\STATUS_INTERNAL, $call->getStatus()->code, "$label stream status");
    grpc_lite_phpt_assert_same('malformed HTTP/2 response header sequence', $call->getStatus()->details, "$label stream details");
};

$assertMalformedUnary($client(), 'trailer-without-end-stream', 'nonterminal trailer');
$assertMalformedStream($client(), 'trailer-without-end-stream', 'nonterminal trailer', 1);
foreach ([
    'informational-end-stream' => '103 END_STREAM',
    'informational-then-missing-status' => '103 then missing status',
    'informational-then-data' => '103 then DATA',
] as $control => $label) {
    $assertMalformedUnary($client(), $control, $label);
    $assertMalformedStream($client(), $control, $label, 0);
}

$assertNonterminalStatusFieldUnary = static function (GreeterClient $client, string $control, string $label): void {
    [$response, $status] = $client->BenchUnary(new BenchRequest(), [
        'x-bench-raw-response' => [$control],
    ], ['timeout' => 2_000_000])->wait();
    grpc_lite_phpt_assert_same(null, $response, "$label unary response");
    grpc_lite_phpt_assert_same(Grpc\STATUS_UNKNOWN, $status->code, "$label unary status");
    grpc_lite_phpt_assert_same('invalid grpc-status trailer', $status->details, "$label unary details");

    [$followUpResponse, $followUpStatus] = $client->BenchUnary(new BenchRequest(), [
        'x-bench-raw-response' => ['require-prior-status-probe'],
    ])->wait();
    grpc_lite_phpt_assert_true($followUpResponse instanceof \Helloworld\BenchReply, "$label unary follow-up response");
    grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $followUpStatus->code, "$label unary follow-up status");
};

$assertNonterminalStatusFieldStream = static function (GreeterClient $client, string $control, string $label): void {
    $request = new BenchRequest();
    $request->setMessageCount(1);
    $call = $client->BenchServerStream($request, [
        'x-bench-raw-response' => [$control],
    ], ['timeout' => 2_000_000]);
    $count = 0;
    foreach ($call->responses() as $_reply) {
        $count++;
    }
    grpc_lite_phpt_assert_same(0, $count, "$label stream count");
    grpc_lite_phpt_assert_same(Grpc\STATUS_UNKNOWN, $call->getStatus()->code, "$label stream status");
    grpc_lite_phpt_assert_same('invalid grpc-status trailer', $call->getStatus()->details, "$label stream details");

    $followUpRequest = new BenchRequest();
    $followUpRequest->setMessageCount(1);
    $followUp = $client->BenchServerStream($followUpRequest, [
        'x-bench-raw-response' => ['require-prior-status-probe'],
    ]);
    $followUpCount = 0;
    foreach ($followUp->responses() as $_reply) {
        $followUpCount++;
    }
    grpc_lite_phpt_assert_same(1, $followUpCount, "$label stream follow-up count");
    grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $followUp->getStatus()->code, "$label stream follow-up status");
};

foreach ([
    'post-informational-silent-grpc-status' => 'silent nonterminal grpc-status',
    'post-informational-silent-grpc-message' => 'silent nonterminal grpc-message',
    'post-informational-silent-status-details' => 'silent nonterminal grpc-status-details-bin',
] as $control => $label) {
    $assertNonterminalStatusFieldUnary($client(), $control, $label);
    $assertNonterminalStatusFieldStream($client(), $control, $label);
}

file_put_contents($traceFile, '');
$assertIncompleteUnary = static function (
    GreeterClient $client,
    string $control,
    string $label,
    int $expectedStatus,
    string $expectedDetails,
): void {
    $startedNs = hrtime(true);
    [$response, $status] = $client->BenchUnary(new BenchRequest(), [
        'x-bench-raw-response' => [$control],
    ])->wait();
    grpc_lite_phpt_assert_same(null, $response, "$label unary response");
    grpc_lite_phpt_assert_same($expectedStatus, $status->code, "$label unary status");
    grpc_lite_phpt_assert_same($expectedDetails, $status->details, "$label unary details");
    grpc_lite_phpt_assert_true(hrtime(true) - $startedNs < 500_000_000, "$label unary is finite without a deadline");

    [$followUpResponse, $followUpStatus] = $client->BenchUnary(new BenchRequest(), [
        'x-bench-raw-response' => ['require-prior-incomplete-status-cancel'],
    ])->wait();
    grpc_lite_phpt_assert_true($followUpResponse instanceof \Helloworld\BenchReply, "$label unary fresh follow-up response");
    grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $followUpStatus->code, "$label unary fresh follow-up status");
};

$assertIncompleteStream = static function (
    GreeterClient $client,
    string $control,
    string $label,
    int $expectedCount,
    int $expectedStatus,
    string $expectedDetails,
): void {
    $request = new BenchRequest();
    $request->setMessageCount(1);
    $startedNs = hrtime(true);
    $call = $client->BenchServerStream($request, [
        'x-bench-raw-response' => [$control],
    ]);
    $count = 0;
    foreach ($call->responses() as $_reply) {
        $count++;
    }
    grpc_lite_phpt_assert_same($expectedCount, $count, "$label stream count");
    grpc_lite_phpt_assert_same($expectedStatus, $call->getStatus()->code, "$label stream status");
    grpc_lite_phpt_assert_same($expectedDetails, $call->getStatus()->details, "$label stream details");
    grpc_lite_phpt_assert_true(hrtime(true) - $startedNs < 500_000_000, "$label stream is finite without a deadline");

    $followUpRequest = new BenchRequest();
    $followUpRequest->setMessageCount(1);
    $followUp = $client->BenchServerStream($followUpRequest, [
        'x-bench-raw-response' => ['require-prior-incomplete-status-cancel'],
    ]);
    $followUpCount = 0;
    foreach ($followUp->responses() as $_reply) {
        $followUpCount++;
    }
    grpc_lite_phpt_assert_same(1, $followUpCount, "$label stream fresh follow-up count");
    grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $followUp->getStatus()->code, "$label stream fresh follow-up status");
};

foreach ([
    ['post-informational-incomplete-grpc-status', 'incomplete grpc-status block', 0, Grpc\STATUS_UNKNOWN, 'invalid grpc-status trailer'],
    ['post-informational-incomplete-grpc-message', 'incomplete grpc-message block', 0, Grpc\STATUS_UNKNOWN, 'invalid grpc-status trailer'],
    ['post-informational-incomplete-status-details', 'incomplete grpc-status-details-bin block', 0, Grpc\STATUS_UNKNOWN, 'invalid grpc-status trailer'],
    ['incomplete-informational-end-stream', 'incomplete 103 END_STREAM block', 0, Grpc\STATUS_INTERNAL, 'malformed HTTP/2 response header sequence'],
    ['incomplete-trailer-without-end-stream', 'incomplete nonterminal trailer block', 1, Grpc\STATUS_INTERNAL, 'malformed HTTP/2 response header sequence'],
    ['post-informational-incomplete-regular-before-status', 'incomplete regular-before-status block', 0, Grpc\STATUS_INTERNAL, 'malformed HTTP/2 response header sequence'],
    ['post-informational-incomplete-invalid-before-status', 'incomplete invalid-regular-before-status block', 0, Grpc\STATUS_INTERNAL, 'malformed HTTP/2 response header sequence'],
    ['post-informational-incomplete-empty-name-before-status', 'incomplete empty-name-before-status block', 0, Grpc\STATUS_INTERNAL, 'malformed HTTP/2 response header sequence'],
    ['post-informational-incomplete-strict-invalid-pseudo-before-status', 'incomplete strict-invalid-pseudo-before-status block', 0, Grpc\STATUS_INTERNAL, 'malformed HTTP/2 response header sequence'],
    ['post-informational-incomplete-uppercase-regular-before-status', 'incomplete uppercase-regular-before-status block', 0, Grpc\STATUS_INTERNAL, 'malformed HTTP/2 response header sequence'],
    ['informational-incomplete-entry-budget', 'incomplete informational entry budget block', 0, Grpc\STATUS_RESOURCE_EXHAUSTED, 'response header/metadata budget exceeded'],
    ['informational-incomplete-invalid-entry-budget', 'incomplete invalid informational entry budget block', 0, Grpc\STATUS_RESOURCE_EXHAUSTED, 'response header/metadata budget exceeded'],
    ['partial-data-compressed-message', 'partial DATA compressed message', 0, Grpc\STATUS_INTERNAL, 'compressed gRPC messages are not supported'],
] as [$control, $label, $expectedCount, $expectedStatus, $expectedDetails]) {
    $assertIncompleteUnary($client(), $control, $label, $expectedStatus, $expectedDetails);
    $assertIncompleteStream($client(), $control, $label, $expectedCount, $expectedStatus, $expectedDetails);
}

$incompleteRstFrames = [];
$incompleteConnectionPrefaces = 0;
foreach (file($traceFile, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
    $record = json_decode($line, true);
    grpc_lite_phpt_assert_true(is_array($record), 'incomplete block trace line must be JSON object');
    if (($record['event'] ?? null) === 'wire.frame_out' && ($record['frame_type'] ?? null) === 'RST_STREAM') {
        $incompleteRstFrames[] = $record;
    }
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $incompleteConnectionPrefaces++;
    }
}
grpc_lite_phpt_assert_same(26, count($incompleteRstFrames), 'incomplete receive traces contain one RST_STREAM each');
$incompleteRstCodes = array_count_values(array_map(
    static fn (array $frame): mixed => $frame['error_code'] ?? null,
    $incompleteRstFrames,
));
grpc_lite_phpt_assert_same(14, $incompleteRstCodes[1] ?? 0, 'incomplete malformed blocks emit PROTOCOL_ERROR');
grpc_lite_phpt_assert_same(12, $incompleteRstCodes[8] ?? 0, 'incomplete status, budget, and DATA cases emit CANCEL');
grpc_lite_phpt_assert_same(52, $incompleteConnectionPrefaces, 'incomplete receive cases quarantine twenty-six connections before follow-up');

$readLateTrace = static function (string $label) use ($traceFile): array {
    $records = [];
    foreach (file($traceFile, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
        $record = json_decode($line, true);
        grpc_lite_phpt_assert_true(is_array($record), "$label trace line must be JSON object");
        $records[] = $record;
    }
    return $records;
};
$assertLateTrace = static function (array $records, string $label, bool $expectUnaryEnd, bool $expectTerminalDestroy): void {
    $prefaces = 0;
    $rstFrames = 0;
    $unaryEnd = null;
    $connectionDestroys = [];
    foreach ($records as $record) {
        if (($record['event'] ?? null) === 'wire.connection_preface') {
            $prefaces++;
        }
        if (($record['event'] ?? null) === 'wire.frame_out'
            && ($record['frame_type'] ?? null) === 'RST_STREAM') {
            $rstFrames++;
        }
        if (($record['event'] ?? null) === 'rpc.end'
            && ($record['rpc_kind'] ?? null) === 'unary') {
            $unaryEnd = $record;
        }
        if (($record['event'] ?? null) === 'transport.connection_destroy') {
            $connectionDestroys[] = $record;
        }
    }
    grpc_lite_phpt_assert_same(1, $prefaces, "$label opens exactly one connection");
    grpc_lite_phpt_assert_same(0, $rstFrames, "$label does not misattribute a RST_STREAM");
    if ($expectUnaryEnd) {
        grpc_lite_phpt_assert_same(false, $unaryEnd['persistent_reused'] ?? null, "$label unary connection is fresh");
    }
    if ($expectTerminalDestroy) {
        grpc_lite_phpt_assert_same(1, count($connectionDestroys), "$label destroys the quarantined connection");
        grpc_lite_phpt_assert_same(true, $connectionDestroys[0]['dead'] ?? null, "$label destroys a dead connection");
    } else {
        grpc_lite_phpt_assert_same(0, count($connectionDestroys), "$label keeps the healthy connection cached");
    }
};

file_put_contents($traceFile, '');
$partialDataClient = $client();
[$partialDataResponse, $partialDataStatus] = $partialDataClient->BenchUnary(new BenchRequest(), [
    'x-bench-raw-response' => ['partial-data-compressed-message'],
])->wait();
grpc_lite_phpt_assert_same(null, $partialDataResponse, 'partial DATA semantic reset response');
grpc_lite_phpt_assert_same(Grpc\STATUS_INTERNAL, $partialDataStatus->code, 'partial DATA semantic reset status');
grpc_lite_phpt_assert_same('compressed gRPC messages are not supported', $partialDataStatus->details, 'partial DATA semantic reset details');
$partialDataRecords = $readLateTrace('partial DATA semantic reset');
$partialDataPrefaces = 0;
$partialDataInboundHeaders = [];
$partialDataInboundData = [];
$partialDataRstFrames = [];
$partialDataEnd = null;
$partialDataConnectionDestroys = [];
foreach ($partialDataRecords as $record) {
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $partialDataPrefaces++;
    }
    if (($record['event'] ?? null) === 'wire.frame_in') {
        if (($record['frame_type'] ?? null) === 'HEADERS') {
            $partialDataInboundHeaders[] = $record;
        }
        if (($record['frame_type'] ?? null) === 'DATA') {
            $partialDataInboundData[] = $record;
        }
    }
    if (($record['event'] ?? null) === 'wire.frame_out'
        && ($record['frame_type'] ?? null) === 'RST_STREAM') {
        $partialDataRstFrames[] = $record;
    }
    if (($record['event'] ?? null) === 'rpc.end'
        && ($record['rpc_kind'] ?? null) === 'unary') {
        $partialDataEnd = $record;
    }
    if (($record['event'] ?? null) === 'transport.connection_destroy') {
        $partialDataConnectionDestroys[] = $record;
    }
}
grpc_lite_phpt_assert_same(1, $partialDataPrefaces, 'partial DATA semantic reset opens one connection');
grpc_lite_phpt_assert_same(1, count($partialDataInboundHeaders), 'partial DATA semantic reset completes initial HEADERS');
grpc_lite_phpt_assert_same(0, count($partialDataInboundData), 'partial DATA semantic reset never completes inbound DATA');
grpc_lite_phpt_assert_same(1, count($partialDataRstFrames), 'partial DATA semantic reset emits one RST_STREAM');
grpc_lite_phpt_assert_same(8, $partialDataRstFrames[0]['error_code'] ?? null, 'partial DATA semantic reset RST_STREAM is CANCEL');
grpc_lite_phpt_assert_same('/helloworld.Greeter/BenchUnary', $partialDataRstFrames[0]['rpc_method'] ?? null, 'partial DATA semantic reset RST_STREAM belongs to target');
grpc_lite_phpt_assert_same(Grpc\STATUS_INTERNAL, $partialDataEnd['status_code'] ?? null, 'partial DATA semantic reset rpc.end keeps INTERNAL');
grpc_lite_phpt_assert_same(1, count($partialDataConnectionDestroys), 'partial DATA semantic reset destroys connection before follow-up');
grpc_lite_phpt_assert_same(true, $partialDataConnectionDestroys[0]['dead'] ?? null, 'partial DATA semantic reset destroys a dead connection');

file_put_contents($traceFile, '');
[$partialDataFollowUpResponse, $partialDataFollowUpStatus] = $partialDataClient->BenchUnary(new BenchRequest(), [
    'x-bench-raw-response' => ['require-prior-incomplete-status-cancel'],
])->wait();
grpc_lite_phpt_assert_true($partialDataFollowUpResponse instanceof \Helloworld\BenchReply, 'partial DATA semantic reset fresh follow-up response');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $partialDataFollowUpStatus->code, 'partial DATA semantic reset fresh follow-up status');
$partialDataFollowUpRecords = $readLateTrace('partial DATA semantic reset follow-up');
$partialDataFollowUpPrefaces = 0;
$partialDataFollowUpEnd = null;
foreach ($partialDataFollowUpRecords as $record) {
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $partialDataFollowUpPrefaces++;
    }
    if (($record['event'] ?? null) === 'rpc.end'
        && ($record['rpc_kind'] ?? null) === 'unary') {
        $partialDataFollowUpEnd = $record;
    }
}
grpc_lite_phpt_assert_same(1, $partialDataFollowUpPrefaces, 'partial DATA semantic reset follow-up opens fresh connection');
grpc_lite_phpt_assert_same(false, $partialDataFollowUpEnd['persistent_reused'] ?? null, 'partial DATA semantic reset follow-up does not reuse poisoned connection');

$lateUnaryClient = $client();
file_put_contents($traceFile, '');
[$lateUnaryResponse, $lateUnaryStatus] = $lateUnaryClient->BenchUnary(new BenchRequest(), [
    'x-bench-raw-response' => ['late-incomplete-headers-after-close'],
])->wait();
grpc_lite_phpt_assert_true($lateUnaryResponse instanceof \Helloworld\BenchReply, 'late closed-stream unary target response');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $lateUnaryStatus->code, 'late closed-stream unary target keeps OK');
$assertLateTrace($readLateTrace('late closed-stream unary target'), 'late closed-stream unary target', true, true);

file_put_contents($traceFile, '');
[$lateUnaryFollowUpResponse, $lateUnaryFollowUpStatus] = $lateUnaryClient->BenchUnary(new BenchRequest())->wait();
grpc_lite_phpt_assert_true($lateUnaryFollowUpResponse instanceof \Helloworld\BenchReply, 'late closed-stream unary fresh follow-up response');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $lateUnaryFollowUpStatus->code, 'late closed-stream unary fresh follow-up status');
$assertLateTrace($readLateTrace('late closed-stream unary follow-up'), 'late closed-stream unary follow-up', true, false);

$lateStreamClient = $client();
$lateStreamRequest = new BenchRequest();
$lateStreamRequest->setMessageCount(1);
file_put_contents($traceFile, '');
$lateStreamCall = $lateStreamClient->BenchServerStream($lateStreamRequest, [
    'x-bench-raw-response' => ['late-incomplete-headers-after-close'],
]);
$lateStreamCount = 0;
foreach ($lateStreamCall->responses() as $_reply) {
    $lateStreamCount++;
}
grpc_lite_phpt_assert_same(1, $lateStreamCount, 'late closed-stream server streaming target message count');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $lateStreamCall->getStatus()->code, 'late closed-stream server streaming target keeps OK');
$assertLateTrace($readLateTrace('late closed-stream server streaming target'), 'late closed-stream server streaming target', false, true);

file_put_contents($traceFile, '');
[$lateStreamFollowUpResponse, $lateStreamFollowUpStatus] = $lateStreamClient->BenchUnary(new BenchRequest())->wait();
grpc_lite_phpt_assert_true($lateStreamFollowUpResponse instanceof \Helloworld\BenchReply, 'late closed-stream streaming-client fresh follow-up response');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $lateStreamFollowUpStatus->code, 'late closed-stream streaming-client fresh follow-up status');
$assertLateTrace($readLateTrace('late closed-stream streaming-client follow-up'), 'late closed-stream streaming-client follow-up', true, false);

file_put_contents($traceFile, '');
$abandonedClient = $client();
[$abandonedResponse, $abandonedStatus] = $abandonedClient->BenchUnary(new BenchRequest(), [
    'x-bench-raw-response' => ['live-incomplete-informational-deadline'],
], ['timeout' => 300_000])->wait();
grpc_lite_phpt_assert_same(null, $abandonedResponse, 'live incomplete block deadline response');
grpc_lite_phpt_assert_same(Grpc\STATUS_DEADLINE_EXCEEDED, $abandonedStatus->code, 'live incomplete block deadline status');
grpc_lite_phpt_assert_same('HTTP/2 transport deadline exceeded', $abandonedStatus->details, 'live incomplete block deadline details');

$abandonedRecords = $readLateTrace('live incomplete block deadline');
$abandonedPrefaces = 0;
$abandonedRstFrames = [];
$abandonedEnd = null;
$abandonedConnectionDestroys = [];
foreach ($abandonedRecords as $record) {
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $abandonedPrefaces++;
    }
    if (($record['event'] ?? null) === 'wire.frame_out'
        && ($record['frame_type'] ?? null) === 'RST_STREAM') {
        $abandonedRstFrames[] = $record;
    }
    if (($record['event'] ?? null) === 'rpc.end'
        && ($record['rpc_kind'] ?? null) === 'unary') {
        $abandonedEnd = $record;
    }
    if (($record['event'] ?? null) === 'transport.connection_destroy') {
        $abandonedConnectionDestroys[] = $record;
    }
}
grpc_lite_phpt_assert_same(1, $abandonedPrefaces, 'live incomplete block target opens one connection');
grpc_lite_phpt_assert_same(1, count($abandonedRstFrames), 'live incomplete block deadline emits one RST_STREAM');
grpc_lite_phpt_assert_same(8, $abandonedRstFrames[0]['error_code'] ?? null, 'live incomplete block deadline RST_STREAM is CANCEL');
grpc_lite_phpt_assert_same('/helloworld.Greeter/BenchUnary', $abandonedRstFrames[0]['rpc_method'] ?? null, 'live incomplete block deadline RST_STREAM belongs to target');
grpc_lite_phpt_assert_same(Grpc\STATUS_DEADLINE_EXCEEDED, $abandonedEnd['status_code'] ?? null, 'live incomplete block rpc.end keeps deadline status');
grpc_lite_phpt_assert_same(false, $abandonedEnd['persistent_reused'] ?? null, 'live incomplete block target opened the connection');
grpc_lite_phpt_assert_same(1, count($abandonedConnectionDestroys), 'live incomplete block destroys abandoned connection');
grpc_lite_phpt_assert_same(true, $abandonedConnectionDestroys[0]['dead'] ?? null, 'live incomplete block destroys a dead connection');

file_put_contents($traceFile, '');
[$abandonedFollowUpResponse, $abandonedFollowUpStatus] = $abandonedClient->BenchUnary(new BenchRequest(), [
    'x-bench-raw-response' => ['require-prior-incomplete-status-cancel'],
])->wait();
grpc_lite_phpt_assert_true($abandonedFollowUpResponse instanceof \Helloworld\BenchReply, 'live incomplete block fresh follow-up response');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $abandonedFollowUpStatus->code, 'live incomplete block fresh follow-up status');
$abandonedFollowUpRecords = $readLateTrace('live incomplete block follow-up');
$abandonedFollowUpPrefaces = 0;
$abandonedFollowUpEnd = null;
foreach ($abandonedFollowUpRecords as $record) {
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $abandonedFollowUpPrefaces++;
    }
    if (($record['event'] ?? null) === 'rpc.end'
        && ($record['rpc_kind'] ?? null) === 'unary') {
        $abandonedFollowUpEnd = $record;
    }
}
grpc_lite_phpt_assert_same(1, $abandonedFollowUpPrefaces, 'live incomplete block follow-up opens fresh connection');
grpc_lite_phpt_assert_same(false, $abandonedFollowUpEnd['persistent_reused'] ?? null, 'live incomplete block follow-up does not reuse poisoned connection');

file_put_contents($traceFile, '');
$partialFrameClient = $client();
[$partialFrameResponse, $partialFrameStatus] = $partialFrameClient->BenchUnary(new BenchRequest(), [
    'x-bench-raw-response' => ['partial-headers-payload-deadline'],
], ['timeout' => 300_000])->wait();
grpc_lite_phpt_assert_same(null, $partialFrameResponse, 'partial HEADERS payload deadline response');
grpc_lite_phpt_assert_same(Grpc\STATUS_DEADLINE_EXCEEDED, $partialFrameStatus->code, 'partial HEADERS payload deadline status');
grpc_lite_phpt_assert_same('HTTP/2 transport deadline exceeded', $partialFrameStatus->details, 'partial HEADERS payload deadline details');

$partialFrameRecords = $readLateTrace('partial HEADERS payload deadline');
$partialFramePrefaces = 0;
$partialFrameInboundHeaders = [];
$partialFrameRstFrames = [];
$partialFrameEnd = null;
$partialFrameConnectionDestroys = [];
foreach ($partialFrameRecords as $record) {
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $partialFramePrefaces++;
    }
    if (($record['event'] ?? null) === 'wire.frame_in'
        && ($record['frame_type'] ?? null) === 'HEADERS') {
        $partialFrameInboundHeaders[] = $record;
    }
    if (($record['event'] ?? null) === 'wire.frame_out'
        && ($record['frame_type'] ?? null) === 'RST_STREAM') {
        $partialFrameRstFrames[] = $record;
    }
    if (($record['event'] ?? null) === 'rpc.end'
        && ($record['rpc_kind'] ?? null) === 'unary') {
        $partialFrameEnd = $record;
    }
    if (($record['event'] ?? null) === 'transport.connection_destroy') {
        $partialFrameConnectionDestroys[] = $record;
    }
}
grpc_lite_phpt_assert_same(1, $partialFramePrefaces, 'partial HEADERS payload target opens one connection');
grpc_lite_phpt_assert_same(0, count($partialFrameInboundHeaders), 'partial HEADERS payload never completes an inbound HEADERS frame');
grpc_lite_phpt_assert_same(1, count($partialFrameRstFrames), 'partial HEADERS payload deadline emits one RST_STREAM');
grpc_lite_phpt_assert_same(8, $partialFrameRstFrames[0]['error_code'] ?? null, 'partial HEADERS payload deadline RST_STREAM is CANCEL');
grpc_lite_phpt_assert_same('/helloworld.Greeter/BenchUnary', $partialFrameRstFrames[0]['rpc_method'] ?? null, 'partial HEADERS payload RST_STREAM belongs to target');
grpc_lite_phpt_assert_same(Grpc\STATUS_DEADLINE_EXCEEDED, $partialFrameEnd['status_code'] ?? null, 'partial HEADERS payload rpc.end keeps deadline status');
grpc_lite_phpt_assert_same(false, $partialFrameEnd['persistent_reused'] ?? null, 'partial HEADERS payload target opened the connection');
grpc_lite_phpt_assert_same(1, count($partialFrameConnectionDestroys), 'partial HEADERS payload destroys abandoned connection');
grpc_lite_phpt_assert_same(true, $partialFrameConnectionDestroys[0]['dead'] ?? null, 'partial HEADERS payload destroys a dead connection');

file_put_contents($traceFile, '');
[$partialFrameFollowUpResponse, $partialFrameFollowUpStatus] = $partialFrameClient->BenchUnary(new BenchRequest(), [
    'x-bench-raw-response' => ['require-prior-incomplete-status-cancel'],
], ['timeout' => 2_000_000])->wait();
grpc_lite_phpt_assert_true($partialFrameFollowUpResponse instanceof \Helloworld\BenchReply, 'partial HEADERS payload fresh follow-up response');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $partialFrameFollowUpStatus->code, 'partial HEADERS payload fresh follow-up status');
$partialFrameFollowUpRecords = $readLateTrace('partial HEADERS payload follow-up');
$partialFrameFollowUpPrefaces = 0;
$partialFrameFollowUpEnd = null;
foreach ($partialFrameFollowUpRecords as $record) {
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $partialFrameFollowUpPrefaces++;
    }
    if (($record['event'] ?? null) === 'rpc.end'
        && ($record['rpc_kind'] ?? null) === 'unary') {
        $partialFrameFollowUpEnd = $record;
    }
}
grpc_lite_phpt_assert_same(1, $partialFrameFollowUpPrefaces, 'partial HEADERS payload follow-up opens fresh connection');
grpc_lite_phpt_assert_same(false, $partialFrameFollowUpEnd['persistent_reused'] ?? null, 'partial HEADERS payload follow-up does not reuse poisoned connection');

file_put_contents($traceFile, '');
$cleanBoundaryClient = $client();
[$cleanBoundaryResponse, $cleanBoundaryStatus] = $cleanBoundaryClient->BenchUnary(new BenchRequest(), [
    'x-bench-raw-response' => ['clean-headers-boundary-deadline'],
], ['timeout' => 300_000])->wait();
grpc_lite_phpt_assert_same(null, $cleanBoundaryResponse, 'clean HEADERS boundary deadline response');
grpc_lite_phpt_assert_same(Grpc\STATUS_DEADLINE_EXCEEDED, $cleanBoundaryStatus->code, 'clean HEADERS boundary deadline status');
grpc_lite_phpt_assert_same('HTTP/2 transport deadline exceeded', $cleanBoundaryStatus->details, 'clean HEADERS boundary deadline details');

$cleanBoundaryRecords = $readLateTrace('clean HEADERS boundary deadline');
$cleanBoundaryPrefaces = 0;
$cleanBoundaryInboundHeaders = [];
$cleanBoundaryRstFrames = [];
$cleanBoundaryEnd = null;
$cleanBoundaryConnectionDestroys = [];
foreach ($cleanBoundaryRecords as $record) {
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $cleanBoundaryPrefaces++;
    }
    if (($record['event'] ?? null) === 'wire.frame_in'
        && ($record['frame_type'] ?? null) === 'HEADERS') {
        $cleanBoundaryInboundHeaders[] = $record;
    }
    if (($record['event'] ?? null) === 'wire.frame_out'
        && ($record['frame_type'] ?? null) === 'RST_STREAM') {
        $cleanBoundaryRstFrames[] = $record;
    }
    if (($record['event'] ?? null) === 'rpc.end'
        && ($record['rpc_kind'] ?? null) === 'unary') {
        $cleanBoundaryEnd = $record;
    }
    if (($record['event'] ?? null) === 'transport.connection_destroy') {
        $cleanBoundaryConnectionDestroys[] = $record;
    }
}
grpc_lite_phpt_assert_same(1, $cleanBoundaryPrefaces, 'clean HEADERS boundary target opens one connection');
grpc_lite_phpt_assert_same(1, count($cleanBoundaryInboundHeaders), 'clean HEADERS boundary completes one inbound HEADERS frame');
grpc_lite_phpt_assert_same(4, (($cleanBoundaryInboundHeaders[0]['flags'] ?? 0) & 4), 'clean HEADERS boundary frame has END_HEADERS');
grpc_lite_phpt_assert_same(1, count($cleanBoundaryRstFrames), 'clean HEADERS boundary deadline emits one RST_STREAM');
grpc_lite_phpt_assert_same(8, $cleanBoundaryRstFrames[0]['error_code'] ?? null, 'clean HEADERS boundary deadline RST_STREAM is CANCEL');
grpc_lite_phpt_assert_same('/helloworld.Greeter/BenchUnary', $cleanBoundaryRstFrames[0]['rpc_method'] ?? null, 'clean HEADERS boundary RST_STREAM belongs to target');
grpc_lite_phpt_assert_same(Grpc\STATUS_DEADLINE_EXCEEDED, $cleanBoundaryEnd['status_code'] ?? null, 'clean HEADERS boundary rpc.end keeps deadline status');
grpc_lite_phpt_assert_same(false, $cleanBoundaryEnd['persistent_reused'] ?? null, 'clean HEADERS boundary target opened the connection');
grpc_lite_phpt_assert_same(0, count($cleanBoundaryConnectionDestroys), 'clean HEADERS boundary keeps connection cached');

file_put_contents($traceFile, '');
[$cleanBoundaryFollowUpResponse, $cleanBoundaryFollowUpStatus] = $cleanBoundaryClient->BenchUnary(new BenchRequest(), [
    'x-bench-raw-response' => ['require-prior-clean-boundary-cancel'],
], ['timeout' => 2_000_000])->wait();
grpc_lite_phpt_assert_true($cleanBoundaryFollowUpResponse instanceof \Helloworld\BenchReply, 'clean HEADERS boundary same-connection follow-up response');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $cleanBoundaryFollowUpStatus->code, 'clean HEADERS boundary same-connection follow-up status');
$cleanBoundaryFollowUpRecords = $readLateTrace('clean HEADERS boundary follow-up');
$cleanBoundaryFollowUpPrefaces = 0;
$cleanBoundaryFollowUpEnd = null;
$cleanBoundaryFollowUpConnectionDestroys = [];
foreach ($cleanBoundaryFollowUpRecords as $record) {
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $cleanBoundaryFollowUpPrefaces++;
    }
    if (($record['event'] ?? null) === 'rpc.end'
        && ($record['rpc_kind'] ?? null) === 'unary') {
        $cleanBoundaryFollowUpEnd = $record;
    }
    if (($record['event'] ?? null) === 'transport.connection_destroy') {
        $cleanBoundaryFollowUpConnectionDestroys[] = $record;
    }
}
grpc_lite_phpt_assert_same(0, $cleanBoundaryFollowUpPrefaces, 'clean HEADERS boundary follow-up does not open a new connection');
grpc_lite_phpt_assert_same(true, $cleanBoundaryFollowUpEnd['persistent_reused'] ?? null, 'clean HEADERS boundary follow-up reuses clean connection');
grpc_lite_phpt_assert_same(0, count($cleanBoundaryFollowUpConnectionDestroys), 'clean HEADERS boundary follow-up keeps connection cached');

file_put_contents($traceFile, '');
$latePartialHeaderAuthority = 'late-partial-frame-header-target.test';
$latePartialHeaderClient = new GreeterClient('test-server:50071', [
    'credentials' => ChannelCredentials::createInsecure(),
    'grpc.default_authority' => $latePartialHeaderAuthority,
]);
[$latePartialHeaderResponse, $latePartialHeaderStatus] = $latePartialHeaderClient->BenchUnary(new BenchRequest(), [
    'x-bench-raw-response' => ['late-partial-frame-header-after-clean-cancel'],
], ['timeout' => 300_000])->wait();
grpc_lite_phpt_assert_same(null, $latePartialHeaderResponse, 'late partial frame-header target response');
grpc_lite_phpt_assert_same(Grpc\STATUS_DEADLINE_EXCEEDED, $latePartialHeaderStatus->code, 'late partial frame-header target status');
grpc_lite_phpt_assert_same('HTTP/2 transport deadline exceeded', $latePartialHeaderStatus->details, 'late partial frame-header target details');

$latePartialHeaderRecords = $readLateTrace('late partial frame-header target');
$latePartialHeaderPrefaces = 0;
$latePartialHeaderRstFrames = [];
$latePartialHeaderConnectionDestroys = [];
foreach ($latePartialHeaderRecords as $record) {
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $latePartialHeaderPrefaces++;
    }
    if (($record['event'] ?? null) === 'wire.frame_out'
        && ($record['frame_type'] ?? null) === 'RST_STREAM') {
        $latePartialHeaderRstFrames[] = $record;
    }
    if (($record['event'] ?? null) === 'transport.connection_destroy') {
        $latePartialHeaderConnectionDestroys[] = $record;
    }
}
grpc_lite_phpt_assert_same(1, $latePartialHeaderPrefaces, 'late partial frame-header target opens one connection');
grpc_lite_phpt_assert_same(1, count($latePartialHeaderRstFrames), 'late partial frame-header target emits one RST_STREAM');
grpc_lite_phpt_assert_same(8, $latePartialHeaderRstFrames[0]['error_code'] ?? null, 'late partial frame-header target RST_STREAM is CANCEL');
grpc_lite_phpt_assert_same(0, count($latePartialHeaderConnectionDestroys), 'late partial frame-header target is clean when cancelled');

// Use another authority/cache identity as an out-of-band barrier. The fixture
// returns only after observing exact CANCEL and TIOCOUTQ confirms that the
// eight-byte prefix reached the target client's kernel receive queue.
$latePartialHeaderBarrierClient = $client();
[$latePartialHeaderBarrierResponse, $latePartialHeaderBarrierStatus] = $latePartialHeaderBarrierClient->BenchUnary(new BenchRequest(), [
    'x-bench-raw-response' => ['await-late-partial-frame-header'],
    'x-bench-probe-authority' => [$latePartialHeaderAuthority],
], ['timeout' => 2_000_000])->wait();
grpc_lite_phpt_assert_true($latePartialHeaderBarrierResponse instanceof \Helloworld\BenchReply, 'late partial frame-header barrier response');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $latePartialHeaderBarrierStatus->code, 'late partial frame-header barrier status');

file_put_contents($traceFile, '');
[$latePartialHeaderFollowUpResponse, $latePartialHeaderFollowUpStatus] = $latePartialHeaderClient->BenchUnary(new BenchRequest(), [], ['timeout' => 2_000_000])->wait();
grpc_lite_phpt_assert_true($latePartialHeaderFollowUpResponse instanceof \Helloworld\BenchReply, 'late partial frame-header fresh follow-up response');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $latePartialHeaderFollowUpStatus->code, 'late partial frame-header fresh follow-up status');
$latePartialHeaderFollowUpRecords = $readLateTrace('late partial frame-header follow-up');
$latePartialHeaderFollowUpPrefaces = 0;
$latePartialHeaderFollowUpPreflightBytes = 0;
$latePartialHeaderFollowUpEnd = null;
$latePartialHeaderFollowUpConnectionDestroys = [];
foreach ($latePartialHeaderFollowUpRecords as $record) {
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $latePartialHeaderFollowUpPrefaces++;
    }
    if (($record['event'] ?? null) === 'wire.socket_preflight_read'
        && ($record['result_len'] ?? 0) > 0) {
        $latePartialHeaderFollowUpPreflightBytes += $record['result_len'];
    }
    if (($record['event'] ?? null) === 'rpc.end'
        && ($record['rpc_kind'] ?? null) === 'unary') {
        $latePartialHeaderFollowUpEnd = $record;
    }
    if (($record['event'] ?? null) === 'transport.connection_destroy') {
        $latePartialHeaderFollowUpConnectionDestroys[] = $record;
    }
}
grpc_lite_phpt_assert_same(1, $latePartialHeaderFollowUpPrefaces, 'late partial frame-header follow-up opens fresh connection');
grpc_lite_phpt_assert_same(8, $latePartialHeaderFollowUpPreflightBytes, 'late partial frame-header preflight consumes exact prefix');
grpc_lite_phpt_assert_same(false, $latePartialHeaderFollowUpEnd['persistent_reused'] ?? null, 'late partial frame-header follow-up rejects poisoned connection');
grpc_lite_phpt_assert_same(1, count($latePartialHeaderFollowUpConnectionDestroys), 'late partial frame-header preflight destroys poisoned connection');
grpc_lite_phpt_assert_same(true, $latePartialHeaderFollowUpConnectionDestroys[0]['dead'] ?? null, 'late partial frame-header preflight marks poisoned connection dead');

file_put_contents($traceFile, '');
$cancelClient = $client();
$cancelRequest = new BenchRequest();
$cancelRequest->setMessageCount(1);
$cancelCall = $cancelClient->BenchServerStream($cancelRequest, [
    'x-bench-raw-response' => ['live-incomplete-trailing-explicit-cancel'],
]);
$cancelResponses = $cancelCall->responses();
grpc_lite_phpt_assert_true($cancelResponses->current() instanceof \Helloworld\BenchReply, 'live incomplete trailing block yields target message before cancel');
$cancelCall->cancel();
grpc_lite_phpt_assert_same(Grpc\STATUS_CANCELLED, $cancelCall->getStatus()->code, 'live incomplete trailing block explicit cancel status');

$cancelRecords = $readLateTrace('live incomplete trailing block explicit cancel');
$cancelPrefaces = 0;
$cancelRstFrames = [];
$cancelConnectionDestroys = [];
foreach ($cancelRecords as $record) {
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $cancelPrefaces++;
    }
    if (($record['event'] ?? null) === 'wire.frame_out'
        && ($record['frame_type'] ?? null) === 'RST_STREAM') {
        $cancelRstFrames[] = $record;
    }
    if (($record['event'] ?? null) === 'transport.connection_destroy') {
        $cancelConnectionDestroys[] = $record;
    }
}
grpc_lite_phpt_assert_same(1, $cancelPrefaces, 'live incomplete trailing block target opens one connection');
grpc_lite_phpt_assert_same(1, count($cancelRstFrames), 'live incomplete trailing block explicit cancel emits one RST_STREAM');
grpc_lite_phpt_assert_same(8, $cancelRstFrames[0]['error_code'] ?? null, 'live incomplete trailing block explicit cancel RST_STREAM is CANCEL');
grpc_lite_phpt_assert_same('/helloworld.Greeter/BenchServerStream', $cancelRstFrames[0]['rpc_method'] ?? null, 'live incomplete trailing block RST_STREAM belongs to target');
grpc_lite_phpt_assert_same(1, count($cancelConnectionDestroys), 'live incomplete trailing block destroys abandoned connection');
grpc_lite_phpt_assert_same(true, $cancelConnectionDestroys[0]['dead'] ?? null, 'live incomplete trailing block destroys a dead connection');

file_put_contents($traceFile, '');
[$cancelFollowUpResponse, $cancelFollowUpStatus] = $cancelClient->BenchUnary(new BenchRequest(), [
    'x-bench-raw-response' => ['require-prior-incomplete-status-cancel'],
])->wait();
grpc_lite_phpt_assert_true($cancelFollowUpResponse instanceof \Helloworld\BenchReply, 'live incomplete trailing block fresh follow-up response');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $cancelFollowUpStatus->code, 'live incomplete trailing block fresh follow-up status');
$cancelFollowUpRecords = $readLateTrace('live incomplete trailing block follow-up');
$cancelFollowUpPrefaces = 0;
$cancelFollowUpEnd = null;
foreach ($cancelFollowUpRecords as $record) {
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $cancelFollowUpPrefaces++;
    }
    if (($record['event'] ?? null) === 'rpc.end'
        && ($record['rpc_kind'] ?? null) === 'unary') {
        $cancelFollowUpEnd = $record;
    }
}
grpc_lite_phpt_assert_same(1, $cancelFollowUpPrefaces, 'live incomplete trailing block follow-up opens fresh connection');
grpc_lite_phpt_assert_same(false, $cancelFollowUpEnd['persistent_reused'] ?? null, 'live incomplete trailing block follow-up does not reuse poisoned connection');

file_put_contents($traceFile, '');
$multiplexClient = $client();
$siblingRequest = new BenchRequest();
$siblingRequest->setMessageCount(1);
$siblingRequest->setRequestPayload(str_repeat("\0", 16 * 1024 * 1024));
$siblingCall = $multiplexClient->BenchServerStream($siblingRequest, [
    'x-bench-raw-response' => ['multiplex-hold-sibling'],
]);
$siblingResponses = $siblingCall->responses();
grpc_lite_phpt_assert_true($siblingResponses->current() instanceof \Helloworld\BenchReply, 'terminal quarantine sibling first message');

[$targetResponse, $targetStatus] = $multiplexClient->BenchUnary(new BenchRequest(), [
    'x-bench-raw-response' => ['multiplex-incomplete-entry-budget'],
])->wait();
grpc_lite_phpt_assert_same(null, $targetResponse, 'terminal quarantine target response');
grpc_lite_phpt_assert_same(Grpc\STATUS_RESOURCE_EXHAUSTED, $targetStatus->code, 'terminal quarantine target status');
grpc_lite_phpt_assert_same('response header/metadata budget exceeded', $targetStatus->details, 'terminal quarantine target details');

$siblingResponses->next();
grpc_lite_phpt_assert_same(false, $siblingResponses->valid(), 'terminal quarantine sibling has no further message');
grpc_lite_phpt_assert_same(Grpc\STATUS_UNAVAILABLE, $siblingCall->getStatus()->code, 'terminal quarantine sibling status');
grpc_lite_phpt_assert_same('incomplete HTTP/2 receive boundary', $siblingCall->getStatus()->details, 'terminal quarantine sibling details');
$multiplexRecords = [];
foreach (file($traceFile, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
    $record = json_decode($line, true);
    grpc_lite_phpt_assert_true(is_array($record), 'terminal quarantine trace line must be JSON object');
    $multiplexRecords[] = $record;
}
$targetEndIndex = null;
$targetEndUs = null;
$targetStreamId = null;
$targetRstIndex = null;
$targetRstUs = null;
$siblingStreamId = null;
$multiplexRstFrames = [];
$multiplexConnectionPrefaces = 0;
foreach ($multiplexRecords as $index => $record) {
    if (($record['event'] ?? null) === 'wire.request_header'
        && ($record['rpc_method'] ?? null) === '/helloworld.Greeter/BenchServerStream') {
        $siblingStreamId = $record['stream_id'] ?? null;
    }
    if (($record['event'] ?? null) === 'wire.request_header'
        && ($record['rpc_method'] ?? null) === '/helloworld.Greeter/BenchUnary') {
        $targetStreamId = $record['stream_id'] ?? null;
    }
    if (($record['event'] ?? null) === 'rpc.end'
        && ($record['rpc_kind'] ?? null) === 'unary'
        && ($record['status_code'] ?? null) === Grpc\STATUS_RESOURCE_EXHAUSTED) {
        $targetEndIndex = $index;
        $targetEndUs = $record['monotonic_us'] ?? null;
        grpc_lite_phpt_assert_same(true, $record['persistent_reused'] ?? null, 'terminal quarantine target reused sibling connection');
    }
    if (($record['event'] ?? null) === 'wire.frame_out' && ($record['frame_type'] ?? null) === 'RST_STREAM') {
        $multiplexRstFrames[] = $record;
        if (($record['stream_id'] ?? null) === $targetStreamId) {
            $targetRstIndex = $index;
            $targetRstUs = $record['monotonic_us'] ?? null;
        }
    }
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $multiplexConnectionPrefaces++;
    }
}
grpc_lite_phpt_assert_true($targetEndIndex !== null, 'terminal quarantine target rpc.end exists');
grpc_lite_phpt_assert_true(is_int($targetEndUs), 'terminal quarantine target rpc.end timestamp exists');
grpc_lite_phpt_assert_true(is_int($targetStreamId), 'terminal quarantine target stream id exists');
grpc_lite_phpt_assert_true(is_int($targetRstIndex), 'terminal quarantine target RST_STREAM index exists');
grpc_lite_phpt_assert_true(is_int($targetRstUs), 'terminal quarantine target RST_STREAM timestamp exists');
grpc_lite_phpt_assert_true(is_int($siblingStreamId), 'terminal quarantine sibling stream id exists');
grpc_lite_phpt_assert_same(1, count($multiplexRstFrames), 'terminal quarantine emits one target RST_STREAM');
grpc_lite_phpt_assert_same($targetStreamId, $multiplexRstFrames[0]['stream_id'] ?? null, 'terminal quarantine RST_STREAM belongs to target');
grpc_lite_phpt_assert_same(8, $multiplexRstFrames[0]['error_code'] ?? null, 'terminal quarantine target RST_STREAM is CANCEL');
grpc_lite_phpt_assert_same(1, $multiplexConnectionPrefaces, 'terminal quarantine target shares sibling connection');
grpc_lite_phpt_assert_true($targetEndUs >= $targetRstUs, 'terminal quarantine target ends after RST classification');
grpc_lite_phpt_assert_true(
    $targetEndUs - $targetRstUs < 500_000,
    'deadline-less terminal quarantine completes within the fixed grace after target RST classification',
);

foreach ($multiplexRecords as $index => $record) {
    if ($index > $targetRstIndex
        && ($record['event'] ?? null) === 'wire.frame_out'
        && ($record['frame_type'] ?? null) === 'DATA'
        && ($record['stream_id'] ?? null) === $siblingStreamId) {
        throw new RuntimeException('sibling DATA generated after terminal quarantine target RST');
    }
}

$wireEvents = [
    'wire.socket_write',
    'wire.tls_write',
    'wire.tls_write_retry',
    'wire.socket_read',
    'wire.tls_read',
    'wire.tls_read_retry',
    'wire.socket_preflight_read',
    'wire.tls_preflight_read',
    'wire.tls_preflight_read_retry',
    'wire.frame_out',
];
foreach ($multiplexRecords as $index => $record) {
    if ($index > $targetEndIndex && in_array($record['event'] ?? null, $wireEvents, true)) {
        throw new RuntimeException('unexpected I/O after terminal quarantine: ' . json_encode($record));
    }
}

file_put_contents($traceFile, '');
[$quarantineFollowUpResponse, $quarantineFollowUpStatus] = $multiplexClient->BenchUnary(new BenchRequest(), [
    'x-bench-raw-response' => ['require-prior-incomplete-status-cancel'],
])->wait();
grpc_lite_phpt_assert_true($quarantineFollowUpResponse instanceof \Helloworld\BenchReply, 'peer observed terminal quarantine CANCEL without sibling DATA');
grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $quarantineFollowUpStatus->code, 'peer-observed terminal quarantine follow-up status');
$quarantineFollowUpPrefaces = 0;
$quarantineFollowUpEnd = null;
foreach (file($traceFile, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
    $record = json_decode($line, true);
    grpc_lite_phpt_assert_true(is_array($record), 'terminal quarantine follow-up trace line must be JSON object');
    if (($record['event'] ?? null) === 'wire.connection_preface') {
        $quarantineFollowUpPrefaces++;
    }
    if (($record['event'] ?? null) === 'rpc.end' && ($record['rpc_kind'] ?? null) === 'unary') {
        $quarantineFollowUpEnd = $record;
    }
}
grpc_lite_phpt_assert_same(1, $quarantineFollowUpPrefaces, 'terminal quarantine follow-up opens fresh connection');
grpc_lite_phpt_assert_same(false, $quarantineFollowUpEnd['persistent_reused'] ?? null, 'terminal quarantine follow-up does not reuse terminal connection');

$assertResourceUnary = static function (GreeterClient $client, string $control, string $label): void {
    [$response, $status] = $client->BenchUnary(new BenchRequest(), [
        'x-bench-raw-response' => [$control],
    ], ['timeout' => 2_000_000])->wait();
    grpc_lite_phpt_assert_same(null, $response, "$label unary response");
    grpc_lite_phpt_assert_same(Grpc\STATUS_RESOURCE_EXHAUSTED, $status->code, "$label unary status");
    grpc_lite_phpt_assert_same('response header/metadata budget exceeded', $status->details, "$label unary details");

    [$followUpResponse, $followUpStatus] = $client->BenchUnary(new BenchRequest(), [
        'x-bench-raw-response' => ['require-prior-resource-probe'],
    ])->wait();
    grpc_lite_phpt_assert_true($followUpResponse instanceof \Helloworld\BenchReply, "$label unary follow-up response");
    grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $followUpStatus->code, "$label unary follow-up status");
};

$assertResourceStream = static function (GreeterClient $client, string $control, string $label): void {
    $request = new BenchRequest();
    $request->setMessageCount(1);
    $call = $client->BenchServerStream($request, [
        'x-bench-raw-response' => [$control],
    ], ['timeout' => 2_000_000]);
    $count = 0;
    foreach ($call->responses() as $_reply) {
        $count++;
    }
    grpc_lite_phpt_assert_same(0, $count, "$label stream count");
    grpc_lite_phpt_assert_same(Grpc\STATUS_RESOURCE_EXHAUSTED, $call->getStatus()->code, "$label stream status");
    grpc_lite_phpt_assert_same('response header/metadata budget exceeded', $call->getStatus()->details, "$label stream details");

    $followUpRequest = new BenchRequest();
    $followUpRequest->setMessageCount(1);
    $followUp = $client->BenchServerStream($followUpRequest, [
        'x-bench-raw-response' => ['require-prior-resource-probe'],
    ]);
    $followUpCount = 0;
    foreach ($followUp->responses() as $_reply) {
        $followUpCount++;
    }
    grpc_lite_phpt_assert_same(1, $followUpCount, "$label stream follow-up count");
    grpc_lite_phpt_assert_same(Grpc\STATUS_OK, $followUp->getStatus()->code, "$label stream follow-up status");
};

$assertResourceUnary($client(), 'informational-entry-budget', 'informational entry budget');
$assertResourceStream($client(), 'informational-entry-budget', 'informational entry budget');
$metadataOptions = ['grpc.absolute_max_metadata_size' => 1024];
$assertResourceUnary($client($metadataOptions), 'informational-byte-budget', 'informational byte budget');
$assertResourceStream($client($metadataOptions), 'informational-byte-budget', 'informational byte budget');

// The invalid-header callback trace is a same-block cutoff oracle. With
// :status accounting first, callbacks 1..127 fill the 128-entry budget and
// callback 128 overflows it. Propagating TEMPORAL stops before field 129.
file_put_contents($traceFile, '');
$assertResourceUnary($client(), 'informational-invalid-entry-budget', 'invalid regular entry budget');
$invalidHeaderCallbackCount = 0;
foreach (file($traceFile, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
    $record = json_decode($line, true);
    grpc_lite_phpt_assert_true(is_array($record), 'invalid-header trace line must be JSON object');
    if (($record['event'] ?? null) === 'wire.response_invalid_header') {
        $invalidHeaderCallbackCount++;
    }
}
grpc_lite_phpt_assert_same(128, $invalidHeaderCallbackCount, 'production invalid-header callback TEMPORAL cutoff');
$assertResourceStream($client(), 'informational-invalid-entry-budget', 'invalid regular entry budget');
$assertResourceUnary($client($metadataOptions), 'informational-invalid-byte-budget', 'invalid regular byte budget');
$assertResourceStream($client($metadataOptions), 'informational-invalid-byte-budget', 'invalid regular byte budget');

$invalidUnaryCall = $client()->BenchUnary(new BenchRequest(), [
    'x-bench-raw-response' => ['invalid-status-metadata'],
]);
[$invalidUnaryResponse, $invalidUnaryStatus] = $invalidUnaryCall->wait();
grpc_lite_phpt_assert_same(null, $invalidUnaryResponse, 'invalid status unary response');
grpc_lite_phpt_assert_same(Grpc\STATUS_UNKNOWN, $invalidUnaryStatus->code, 'invalid status unary status');
grpc_lite_phpt_assert_same('invalid grpc-status trailer', $invalidUnaryStatus->details, 'invalid status unary details');
grpc_lite_phpt_assert_true(!array_key_exists('x-before', $invalidUnaryCall->getMetadata()), 'x-before not unary initial metadata');
grpc_lite_phpt_assert_true(!array_key_exists('x-after', $invalidUnaryCall->getMetadata()), 'x-after not unary initial metadata');
grpc_lite_phpt_assert_same(['a'], $invalidUnaryCall->getTrailingMetadata()['x-before'] ?? null, 'x-before unary trailing metadata');
grpc_lite_phpt_assert_same(['b'], $invalidUnaryCall->getTrailingMetadata()['x-after'] ?? null, 'x-after unary trailing metadata');

$invalidStreamRequest = new BenchRequest();
$invalidStreamRequest->setMessageCount(1);
$invalidStreamCall = $client()->BenchServerStream($invalidStreamRequest, [
    'x-bench-raw-response' => ['invalid-status-metadata'],
]);
$invalidStreamCount = 0;
foreach ($invalidStreamCall->responses() as $_reply) {
    $invalidStreamCount++;
}
grpc_lite_phpt_assert_same(0, $invalidStreamCount, 'invalid status stream count');
grpc_lite_phpt_assert_same(Grpc\STATUS_UNKNOWN, $invalidStreamCall->getStatus()->code, 'invalid status stream status');
grpc_lite_phpt_assert_same('invalid grpc-status trailer', $invalidStreamCall->getStatus()->details, 'invalid status stream details');
grpc_lite_phpt_assert_true(!array_key_exists('x-before', $invalidStreamCall->getMetadata()), 'x-before not stream initial metadata');
grpc_lite_phpt_assert_true(!array_key_exists('x-after', $invalidStreamCall->getMetadata()), 'x-after not stream initial metadata');
grpc_lite_phpt_assert_same(['a'], $invalidStreamCall->getTrailingMetadata()['x-before'] ?? null, 'x-before stream trailing metadata');
grpc_lite_phpt_assert_same(['b'], $invalidStreamCall->getTrailingMetadata()['x-after'] ?? null, 'x-after stream trailing metadata');

$assertTerminalStatusFieldMetadataUnary = static function (
    GreeterClient $client,
    string $control,
    string $label,
    string $expectedDetails,
    bool $expectStatusDetails,
): void {
    $call = $client->BenchUnary(new BenchRequest(), [
        'x-bench-raw-response' => [$control],
    ]);
    [$response, $status] = $call->wait();
    grpc_lite_phpt_assert_same(null, $response, "$label unary response");
    grpc_lite_phpt_assert_same(Grpc\STATUS_UNKNOWN, $status->code, "$label unary status");
    grpc_lite_phpt_assert_same($expectedDetails, $status->details, "$label unary details");
    grpc_lite_phpt_assert_true(!array_key_exists('x-before', $call->getMetadata()), "$label x-before not unary initial metadata");
    grpc_lite_phpt_assert_true(!array_key_exists('x-after', $call->getMetadata()), "$label x-after not unary initial metadata");
    grpc_lite_phpt_assert_same(['a'], $call->getTrailingMetadata()['x-before'] ?? null, "$label x-before unary trailing metadata");
    grpc_lite_phpt_assert_same(['b'], $call->getTrailingMetadata()['x-after'] ?? null, "$label x-after unary trailing metadata");
    if ($expectStatusDetails) {
        grpc_lite_phpt_assert_same(["\0"], $call->getTrailingMetadata()['grpc-status-details-bin'] ?? null, "$label unary status details trailing metadata");
    }
};

$assertTerminalStatusFieldMetadataStream = static function (
    GreeterClient $client,
    string $control,
    string $label,
    string $expectedDetails,
    bool $expectStatusDetails,
): void {
    $request = new BenchRequest();
    $request->setMessageCount(1);
    $call = $client->BenchServerStream($request, [
        'x-bench-raw-response' => [$control],
    ]);
    $count = 0;
    foreach ($call->responses() as $_reply) {
        $count++;
    }
    grpc_lite_phpt_assert_same(0, $count, "$label stream count");
    grpc_lite_phpt_assert_same(Grpc\STATUS_UNKNOWN, $call->getStatus()->code, "$label stream status");
    grpc_lite_phpt_assert_same($expectedDetails, $call->getStatus()->details, "$label stream details");
    grpc_lite_phpt_assert_true(!array_key_exists('x-before', $call->getMetadata()), "$label x-before not stream initial metadata");
    grpc_lite_phpt_assert_true(!array_key_exists('x-after', $call->getMetadata()), "$label x-after not stream initial metadata");
    grpc_lite_phpt_assert_same(['a'], $call->getTrailingMetadata()['x-before'] ?? null, "$label x-before stream trailing metadata");
    grpc_lite_phpt_assert_same(['b'], $call->getTrailingMetadata()['x-after'] ?? null, "$label x-after stream trailing metadata");
    if ($expectStatusDetails) {
        grpc_lite_phpt_assert_same(["\0"], $call->getTrailingMetadata()['grpc-status-details-bin'] ?? null, "$label stream status details trailing metadata");
    }
};

foreach ([
    ['post-informational-terminal-message-only-metadata', 'terminal grpc-message-only block', 'detail', false],
    ['post-informational-terminal-status-details-only-metadata', 'terminal grpc-status-details-bin-only block', '', true],
] as [$control, $label, $expectedDetails, $expectStatusDetails]) {
    $assertTerminalStatusFieldMetadataUnary($client(), $control, $label, $expectedDetails, $expectStatusDetails);
    $assertTerminalStatusFieldMetadataStream($client(), $control, $label, $expectedDetails, $expectStatusDetails);
}

unlink($traceFile);
echo "OK\n";
?>
--EXPECT--
OK
