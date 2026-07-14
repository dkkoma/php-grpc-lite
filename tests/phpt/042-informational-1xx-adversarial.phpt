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
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';
grpc_lite_phpt_require_autoload();

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

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
    ])->wait();
    grpc_lite_phpt_assert_same(null, $response, "$label unary response");
    grpc_lite_phpt_assert_same(Grpc\STATUS_UNKNOWN, $status->code, "$label unary status");
    grpc_lite_phpt_assert_same('invalid grpc-status trailer', $status->details, "$label unary details");
};

$assertNonterminalStatusFieldStream = static function (GreeterClient $client, string $control, string $label): void {
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
    grpc_lite_phpt_assert_same('invalid grpc-status trailer', $call->getStatus()->details, "$label stream details");
};

$assertNonterminalStatusFieldUnary(
    $client(),
    'post-informational-nonterminal-status-details',
    'nonterminal grpc-status-details-bin',
);
$assertNonterminalStatusFieldStream(
    $client(),
    'post-informational-nonterminal-status-details',
    'nonterminal grpc-status-details-bin',
);

$assertResourceUnary = static function (GreeterClient $client, string $control, string $label): void {
    [$response, $status] = $client->BenchUnary(new BenchRequest(), [
        'x-bench-raw-response' => [$control],
    ])->wait();
    grpc_lite_phpt_assert_same(null, $response, "$label unary response");
    grpc_lite_phpt_assert_same(Grpc\STATUS_RESOURCE_EXHAUSTED, $status->code, "$label unary status");

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
    ]);
    $count = 0;
    foreach ($call->responses() as $_reply) {
        $count++;
    }
    grpc_lite_phpt_assert_same(0, $count, "$label stream count");
    grpc_lite_phpt_assert_same(Grpc\STATUS_RESOURCE_EXHAUSTED, $call->getStatus()->code, "$label stream status");

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
$assertResourceUnary($client(), 'informational-invalid-entry-budget', 'invalid regular entry budget');
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

echo "OK\n";
?>
--EXPECT--
OK
