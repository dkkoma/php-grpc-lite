<?php
declare(strict_types=1);

namespace PhpGrpcLite\Bench;

use Grpc\ChannelCredentials;
use Helloworld\HelloRequest;
use PhpBench\Attributes as Bench;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;

/**
 * TLS and mTLS unary costs against the local Go test-server.
 *
 * Cold cases construct a client/channel per rev. Warm cases reuse the client
 * created in setUp(), so they represent request-local channel reuse.
 */
#[Bench\BeforeMethods('setUp')]
final class TlsUnaryBench
{
    private const TLS_TARGET = 'test-server:50052';
    private const MTLS_TARGET = 'test-server:50053';
    private const CA_PATH = __DIR__ . '/../poc/test-server/certs/server.crt';
    private const CLIENT_CERT_PATH = __DIR__ . '/../poc/test-server/certs/client.crt';
    private const CLIENT_KEY_PATH = __DIR__ . '/../poc/test-server/certs/client.key';

    private string $rootCerts;
    private string $clientCert;
    private string $clientKey;
    private GreeterClient $tlsClient;
    private GreeterClient $mtlsClient;

    public function setUp(): void
    {
        $this->rootCerts = $this->readFile(self::CA_PATH);
        $this->clientCert = $this->readFile(self::CLIENT_CERT_PATH);
        $this->clientKey = $this->readFile(self::CLIENT_KEY_PATH);

        $this->tlsClient = new GreeterClient(self::TLS_TARGET, [
            'credentials' => ChannelCredentials::createSsl($this->rootCerts),
        ]);
        $this->mtlsClient = new GreeterClient(self::MTLS_TARGET, [
            'credentials' => ChannelCredentials::createSsl($this->rootCerts, $this->clientKey, $this->clientCert),
        ]);
    }

    #[Bench\Revs(20), Bench\Iterations(5), Bench\Warmup(1)]
    public function benchWarmTlsUnary(): void
    {
        $this->assertOk($this->tlsClient);
    }

    #[Bench\Revs(20), Bench\Iterations(5), Bench\Warmup(1)]
    public function benchColdTlsUnary(): void
    {
        $client = new GreeterClient(self::TLS_TARGET, [
            'credentials' => ChannelCredentials::createSsl($this->rootCerts),
        ]);
        $this->assertOk($client);
    }

    #[Bench\Revs(20), Bench\Iterations(5), Bench\Warmup(1)]
    public function benchWarmMtlsUnary(): void
    {
        $this->assertOk($this->mtlsClient);
    }

    #[Bench\Revs(20), Bench\Iterations(5), Bench\Warmup(1)]
    public function benchColdMtlsUnary(): void
    {
        $client = new GreeterClient(self::MTLS_TARGET, [
            'credentials' => ChannelCredentials::createSsl($this->rootCerts, $this->clientKey, $this->clientCert),
        ]);
        $this->assertOk($client);
    }

    private function assertOk(GreeterClient $client): void
    {
        $request = new HelloRequest();
        $request->setName('TLS bench');
        [$response, $status] = $client->SayHello($request)->wait();

        if ($status->code !== \Grpc\STATUS_OK) {
            throw new \RuntimeException("unexpected status {$status->code}: {$status->details}");
        }
        if ($response === null || $response->getMessage() !== 'Hello, TLS bench') {
            throw new \RuntimeException('unexpected TLS bench response');
        }
    }

    private function readFile(string $path): string
    {
        $contents = file_get_contents($path);
        if ($contents === false) {
            throw new \RuntimeException("failed to read $path");
        }
        return $contents;
    }
}
