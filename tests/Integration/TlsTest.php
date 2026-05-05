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

    public function testSayHelloOverTls(): void
    {
        $rootCerts = file_get_contents(self::CA_PATH);
        self::assertNotFalse($rootCerts, 'CA file missing');

        $client = new GreeterClient(self::TARGET, [
            'credentials' => ChannelCredentials::createSsl($rootCerts),
        ]);

        $request = new HelloRequest();
        $request->setName('TLS');
        [$response, $status] = $client->SayHello($request)->wait();

        self::assertSame(\Grpc\STATUS_OK, $status->code);
        self::assertSame('Hello, TLS', $response->getMessage());
    }

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

    public function testRejectsServerCertNotInRootCerts(): void
    {
        // An unrelated CA — the server cert won't validate against it.
        $bogusCa = "-----BEGIN CERTIFICATE-----\n" .
            // A throwaway PEM (real-looking but for an unrelated CN); used
            // only to give native TLS verification a non-matching trust anchor.
            "MIIBkzCCATigAwIBAgIUYWlsAAAAAAAAAAAAAAAAAAAAAFcwCgYIKoZIzj0EAwIw\n" .
            "FjEUMBIGA1UEAwwLbm90LW91ci1jYTAeFw0yNTAxMDEwMDAwMDBaFw0zNTAxMDEw\n" .
            "MDAwMDBaMBYxFDASBgNVBAMMC25vdC1vdXItY2EwWTATBgcqhkjOPQIBBggqhkjO\n" .
            "PQMBBwNCAATMfA0o0hUKt0u4XmM9rGFvE60YkrHRycGRMnCpqQH3p2qDdJUvDk7E\n" .
            "MHmM8Mp/iYxxvxyZc2UrpC0JeJHEHKdPo1MwUTAdBgNVHQ4EFgQU3Pfp5R/aKkO5\n" .
            "j+JjsM8+M2RPoMcwHwYDVR0jBBgwFoAU3Pfp5R/aKkO5j+JjsM8+M2RPoMcwDwYD\n" .
            "VR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAgNHADBEAiBg5BvJ2lXAJ2uxK7n6Ohdu\n" .
            "RUF0nwoUe09mvtQfiQ59rgIgZkzu7cT2hL5fZqL2uOX7wVR2jXcYAB6XL0E8FVWe\n" .
            "n4M=\n" .
            "-----END CERTIFICATE-----\n";

        $client = new GreeterClient(self::TARGET, [
            'credentials' => ChannelCredentials::createSsl($bogusCa),
        ]);

        $request = new HelloRequest();
        $request->setName('Reject');
        [$response, $status] = $client->SayHello($request)->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_UNAVAILABLE, $status->code);
        self::assertNotSame('', $status->details);
    }
}
