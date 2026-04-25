<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\ChannelCredentials;
use Helloworld\HelloReply;
use Helloworld\HelloRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\TestCase;

#[Group('integration')]
final class UnaryTest extends TestCase
{
    private const TARGET = 'test-server:50051';

    public function testSayHelloReturnsExpectedMessage(): void
    {
        $client = new GreeterClient(self::TARGET, [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        $request = new HelloRequest();
        $request->setName('Phase0');

        $call = $client->SayHello($request);
        [$response, $status] = $call->wait();

        self::assertSame(\Grpc\STATUS_OK, $status->code);
        self::assertSame('', $status->details);
        self::assertInstanceOf(HelloReply::class, $response);
        self::assertSame('Hello, Phase0', $response->getMessage());
    }

    public function testSayHelloReturnsReceivedMetadataAndTrailers(): void
    {
        $client = new GreeterClient(self::TARGET, [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        $request = new HelloRequest();
        $request->setName('Meta');

        $call = $client->SayHello($request);
        $call->wait();

        $headers = $call->getMetadata();
        $trailers = $call->getTrailingMetadata();

        self::assertArrayHasKey('content-type', $headers);
        self::assertSame('application/grpc', $headers['content-type'][0]);

        self::assertArrayHasKey('grpc-status', $trailers);
        self::assertSame('0', $trailers['grpc-status'][0]);
    }
}
