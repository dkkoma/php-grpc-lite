<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\ChannelCredentials;
use Helloworld\HelloRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\TestCase;

/**
 * Exercises the SSL code path against the local test server's h2-tls
 * listener (port 50052). The test container mounts the same self-signed
 * CA bundle that the server presents, and we pass it via createSsl().
 */
#[Group('integration')]
final class TlsTest extends TestCase
{
    private const TARGET = 'test-server:50052';
    private const CA_PATH = __DIR__ . '/../../poc/test-server/certs/server.crt';

    public function testSayManyHellosOverTls(): void
    {
        $rootCerts = file_get_contents(self::CA_PATH);

        $client = new GreeterClient(self::TARGET, [
            'credentials' => ChannelCredentials::createSsl($rootCerts),
        ]);

        $request = new HelloRequest();
        $request->setName('TlsStream');

        $messages = [];
        $call = $client->SayManyHellos($request);
        foreach ($call->responses() as $reply) {
            $messages[] = $reply->getMessage();
        }

        self::assertCount(5, $messages);
        self::assertSame('Hello #1, TlsStream', $messages[0]);
        self::assertSame(\Grpc\STATUS_OK, $call->getStatus()->code);
    }
}
