<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\TestCase;

#[Group('integration')]
final class MetadataCompatibilityTest extends TestCase
{
    public function testDuplicateAsciiMetadataRoundTripPreservesValues(): void
    {
        $client = $this->client();
        $values = ['first', 'second', 'third'];

        $call = $client->BenchUnary(new BenchRequest(), [
            'x-bench-echo-ascii' => $values,
            'x-bench-response-duplicate' => $values,
        ]);
        [, $status] = $call->wait();

        self::assertSame(\Grpc\STATUS_OK, $status->code, $status->details);
        self::assertSame($values, $call->getMetadata()['x-bench-initial-ascii'] ?? null);
        self::assertSame($values, $call->getTrailingMetadata()['x-bench-trailing-ascii'] ?? null);
        self::assertSame($values, $call->getMetadata()['x-bench-initial-duplicate'] ?? null);
        self::assertSame($values, $call->getTrailingMetadata()['x-bench-trailing-duplicate'] ?? null);
    }

    public function testDuplicateBinaryMetadataRoundTripPreservesValues(): void
    {
        $client = $this->client();
        $values = ["\x00first", "\x01second,comma", random_bytes(16)];

        $call = $client->BenchUnary(new BenchRequest(), [
            'x-bench-echo-bin' => $values,
            'x-bench-response-duplicate-bin' => $values,
        ]);
        [, $status] = $call->wait();

        self::assertSame(\Grpc\STATUS_OK, $status->code, $status->details);
        self::assertSame($values, $call->getMetadata()['x-bench-initial-bin'] ?? null);
        self::assertSame($values, $call->getTrailingMetadata()['x-bench-trailing-bin'] ?? null);
        self::assertSame($values, $call->getMetadata()['x-bench-initial-duplicate-bin'] ?? null);
        self::assertSame($values, $call->getTrailingMetadata()['x-bench-trailing-duplicate-bin'] ?? null);
    }

    public function testRequestMetadataAllowsManyHeadersAndLargeValues(): void
    {
        $client = $this->client();
        $metadata = [];
        for ($i = 0; $i < 12; $i++) {
            $metadata[sprintf('x-bench-extra-%02d', $i)] = ['extra'];
        }
        $metadata['x-bench-echo-ascii'] = [str_repeat('v', 600)];

        $call = $client->BenchUnary(new BenchRequest(), $metadata);
        [, $status] = $call->wait();

        self::assertSame(\Grpc\STATUS_OK, $status->code, $status->details);
        self::assertSame($metadata['x-bench-echo-ascii'], $call->getMetadata()['x-bench-initial-ascii'] ?? null);
        self::assertSame($metadata['x-bench-echo-ascii'], $call->getTrailingMetadata()['x-bench-trailing-ascii'] ?? null);
    }

    public function testServerStreamingDuplicateMetadataRoundTripPreservesValues(): void
    {
        $client = $this->client();
        $request = new BenchRequest();
        $request->setMessageCount(2);
        $request->setPayloadBytes(10);
        $values = ['stream-first', 'stream-second'];

        $call = $client->BenchServerStream($request, [
            'x-bench-echo-ascii' => $values,
            'x-bench-response-duplicate' => $values,
        ]);
        $count = 0;
        foreach ($call->responses() as $_reply) {
            $count++;
        }

        self::assertSame(2, $count);
        self::assertSame(\Grpc\STATUS_OK, $call->getStatus()->code, $call->getStatus()->details);
        self::assertSame($values, $call->getMetadata()['x-bench-initial-ascii'] ?? null);
        self::assertSame($values, $call->getTrailingMetadata()['x-bench-trailing-ascii'] ?? null);
        self::assertSame($values, $call->getMetadata()['x-bench-initial-duplicate'] ?? null);
        self::assertSame($values, $call->getTrailingMetadata()['x-bench-trailing-duplicate'] ?? null);
    }

    private function client(): GreeterClient
    {
        return new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
    }
}
