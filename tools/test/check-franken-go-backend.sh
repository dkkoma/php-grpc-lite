#!/usr/bin/env bash
#
# Smoke-test php-grpc-lite's optional FrankenPHP grpc-go backend using a
# FrankenPHP binary built with github.com/dkkoma/frankenphp-grpc-go-client.
set -euo pipefail

cd "$(dirname "$0")/../.."

if [[ ! -f vendor/autoload.php ]]; then
    composer install --no-interaction --prefer-dist
fi

cat > /tmp/php-grpc-lite-franken-go-smoke.php <<'PHP'
<?php
declare(strict_types=1);

require 'vendor/autoload.php';

use Grpc\Channel;
use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use Helloworld\HelloRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

if (!extension_loaded('grpc')) {
    throw new RuntimeException('grpc extension is not loaded');
}
if (!class_exists(FrankenGrpc\Channel::class)) {
    throw new RuntimeException('FrankenGrpc\\Channel is not loaded');
}

function assertUnary(string $backend): void
{
    $target = 'test-server:50051';
    $options = [
        'credentials' => ChannelCredentials::createInsecure(),
        'grpc_lite.backend' => $backend,
    ];
    $channel = new Channel($target, $options);
    $client = new GreeterClient($target, $options, $channel);

    $request = new HelloRequest();
    $request->setName('franken-go');
    [$response, $status] = $client->SayHello($request)->wait();
    if ($status->code !== Grpc\STATUS_OK || $response === null) {
        throw new RuntimeException("SayHello failed for backend={$backend}: {$status->details}");
    }

    $channel->close();
}

function assertServerStreaming(string $backend): void
{
    $target = 'test-server:50051';
    $options = [
        'credentials' => ChannelCredentials::createInsecure(),
        'grpc_lite.backend' => $backend,
    ];
    $channel = new Channel($target, $options);
    $client = new GreeterClient($target, $options, $channel);

    $request = new BenchRequest();
    $request->setMessageCount(5);
    $request->setPayloadBytes(32);

    $call = $client->BenchServerStream($request);
    $count = 0;
    foreach ($call->responses() as $reply) {
        $count++;
    }
    $status = $call->getStatus();
    if ($status->code !== Grpc\STATUS_OK) {
        throw new RuntimeException("BenchServerStream failed for backend={$backend}: {$status->details}");
    }
    if ($count !== 5) {
        throw new RuntimeException("BenchServerStream expected 5 responses for backend={$backend}, got {$count}");
    }

    $channel->close();
}

assertUnary('franken-go');
assertServerStreaming('franken-go');
assertUnary('auto');
assertServerStreaming('auto');

echo "FrankenPHP grpc-go backend smoke passed\n";
PHP

tools/frankenphp-grpc-lite-run.sh /tmp/php-grpc-lite-franken-go-smoke.php
