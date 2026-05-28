<?php
declare(strict_types=1);

use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

/**
 * @return array{worker_id:int, case:string, calls:int, messages:int, wall_ns:int, latencies_ns:list<int>}
 */
function zts_parallel_run_worker_case(
    string $autoload,
    string $case,
    string $target,
    int $calls,
    int $warmupCalls,
    int $serverDelayMs,
    int $streamMessages,
    int $payloadBytes,
    int $workerId,
): array {
    require_once $autoload;

    $client = new GreeterClient($target, [
        'credentials' => Grpc\ChannelCredentials::createInsecure(),
    ]);
    $request = zts_parallel_make_request($case, $serverDelayMs, $streamMessages, $payloadBytes);

    for ($index = 0; $index < $warmupCalls; $index++) {
        zts_parallel_run_one_call($case, $client, $request, $streamMessages);
    }

    $latencies = [];
    $messages = 0;
    $started = hrtime(true);
    for ($index = 0; $index < $calls; $index++) {
        $callStarted = hrtime(true);
        $messages += zts_parallel_run_one_call($case, $client, $request, $streamMessages);
        $latencies[] = hrtime(true) - $callStarted;
    }
    $ended = hrtime(true);

    return [
        'worker_id' => $workerId,
        'case' => $case,
        'calls' => $calls,
        'messages' => $messages,
        'wall_ns' => $ended - $started,
        'latencies_ns' => $latencies,
    ];
}

function zts_parallel_make_request(string $case, int $serverDelayMs, int $streamMessages, int $payloadBytes): BenchRequest
{
    $request = new BenchRequest();
    $request->setPayloadBytes($payloadBytes);
    $request->setServerDelayMs($serverDelayMs);
    if ($case === 'streaming') {
        $request->setMessageCount($streamMessages);
    }

    return $request;
}

function zts_parallel_run_one_call(string $case, GreeterClient $client, BenchRequest $request, int $streamMessages): int
{
    if ($case === 'unary') {
        [$response, $status] = $client->BenchUnary($request)->wait();
        if ($status->code !== Grpc\STATUS_OK || $response === null) {
            throw new RuntimeException("BenchUnary failed: {$status->details}");
        }

        return 1;
    }

    $count = 0;
    foreach ($client->BenchServerStream($request)->responses() as $_reply) {
        $count++;
    }
    if ($count !== $streamMessages) {
        throw new RuntimeException("BenchServerStream expected $streamMessages messages, got $count");
    }

    return $count;
}
