<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\ChannelCredentials;
use Helloworld\HelloRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\TestCase;

/**
 * mTLS verification: the test-server's :50053 listener requires the client
 * to present a cert signed by (or equal to) /certs/client.crt. Exercises
 * the certChain / privateKey arguments of ChannelCredentials::createSsl().
 */
#[Group('integration')]
final class MtlsTest extends TestCase
{
    private const TARGET = 'test-server:50053';
    private const CA_PATH = __DIR__ . '/../../poc/test-server/certs/server.crt';

    public function testRejectsConnectionWithoutClientCert(): void
    {
        // Same target, but createSsl without a client cert/key — server
        // should refuse the TLS handshake.
        $rootCerts = file_get_contents(self::CA_PATH);

        $client = new GreeterClient(self::TARGET, [
            'credentials' => ChannelCredentials::createSsl($rootCerts),
        ]);

        $request = new HelloRequest();
        $request->setName('NoCert');
        [$response, $status] = $client->SayHello($request)->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_UNAVAILABLE, $status->code);
        self::assertNotSame('', $status->details);
    }
}
