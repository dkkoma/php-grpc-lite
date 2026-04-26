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
    private const CLIENT_CERT_PATH = __DIR__ . '/../../poc/test-server/certs/client.crt';
    private const CLIENT_KEY_PATH = __DIR__ . '/../../poc/test-server/certs/client.key';

    public function testSayHelloWithClientCertSucceeds(): void
    {
        $rootCerts = file_get_contents(self::CA_PATH);
        $clientCert = file_get_contents(self::CLIENT_CERT_PATH);
        $clientKey = file_get_contents(self::CLIENT_KEY_PATH);
        self::assertNotFalse($rootCerts);
        self::assertNotFalse($clientCert);
        self::assertNotFalse($clientKey);

        $client = new GreeterClient(self::TARGET, [
            'credentials' => ChannelCredentials::createSsl($rootCerts, $clientKey, $clientCert),
        ]);

        $request = new HelloRequest();
        $request->setName('mTLS');
        [$response, $status] = $client->SayHello($request)->wait();

        self::assertSame(\Grpc\STATUS_OK, $status->code, 'mTLS call failed: ' . $status->details);
        self::assertSame('Hello, mTLS', $response->getMessage());
    }

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
        self::assertStringContainsString('curl error', $status->details);
    }
}
