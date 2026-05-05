<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\ChannelCredentials;
use Grpc\Internal\NativeTransport;
use Helloworld\BenchRequest;
use Helloworld\HelloRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;
use PHPUnit\Framework\TestCase;

final class NativeTransportControlTest extends TestCase
{
    private const TLS_TARGET = 'test-server:50052';
    private const MTLS_TARGET = 'test-server:50053';
    private const CA_PATH = __DIR__ . '/../../poc/test-server/certs/server.crt';
    private const CLIENT_CERT_PATH = __DIR__ . '/../../poc/test-server/certs/client.crt';
    private const CLIENT_KEY_PATH = __DIR__ . '/../../poc/test-server/certs/client.key';

    public function testNativeTlsUnarySucceeds(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $rootCerts = file_get_contents(self::CA_PATH);
        self::assertNotFalse($rootCerts);

        $client = new GreeterClient(self::TLS_TARGET, [
            'credentials' => ChannelCredentials::createSsl($rootCerts),
        ]);

        $request = new HelloRequest();
        $request->setName('TLS');
        [$response, $status] = $client->SayHello($request)->wait();

        self::assertSame(\Grpc\STATUS_OK, $status->code, $status->details);
        self::assertSame('Hello, TLS', $response?->getMessage());
    }

    public function testNativeTlsServerStreamingSucceeds(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $rootCerts = file_get_contents(self::CA_PATH);
        self::assertNotFalse($rootCerts);

        $client = new GreeterClient(self::TLS_TARGET, [
            'credentials' => ChannelCredentials::createSsl($rootCerts),
        ]);

        $request = new BenchRequest();
        $request->setMessageCount(3);
        $request->setPayloadBytes(100);

        $call = $client->BenchServerStream($request);
        $count = 0;
        foreach ($call->responses() as $_reply) {
            $count++;
        }

        self::assertSame(3, $count);
        self::assertSame(\Grpc\STATUS_OK, $call->getStatus()->code, $call->getStatus()->details);
    }

    public function testNativeMtlsUnarySucceeds(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $rootCerts = file_get_contents(self::CA_PATH);
        $clientCert = file_get_contents(self::CLIENT_CERT_PATH);
        $clientKey = file_get_contents(self::CLIENT_KEY_PATH);
        self::assertNotFalse($rootCerts);
        self::assertNotFalse($clientCert);
        self::assertNotFalse($clientKey);

        $client = new GreeterClient(self::MTLS_TARGET, [
            'credentials' => ChannelCredentials::createSsl($rootCerts, $clientKey, $clientCert),
        ]);

        $request = new HelloRequest();
        $request->setName('mTLS');
        [$response, $status] = $client->SayHello($request)->wait();

        self::assertSame(\Grpc\STATUS_OK, $status->code, $status->details);
        self::assertSame('Hello, mTLS', $response?->getMessage());
    }

    public function testNativeMtlsServerStreamingSucceeds(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $rootCerts = file_get_contents(self::CA_PATH);
        $clientCert = file_get_contents(self::CLIENT_CERT_PATH);
        $clientKey = file_get_contents(self::CLIENT_KEY_PATH);
        self::assertNotFalse($rootCerts);
        self::assertNotFalse($clientCert);
        self::assertNotFalse($clientKey);

        $client = new GreeterClient(self::MTLS_TARGET, [
            'credentials' => ChannelCredentials::createSsl($rootCerts, $clientKey, $clientCert),
        ]);

        $request = new BenchRequest();
        $request->setMessageCount(3);
        $request->setPayloadBytes(100);

        $call = $client->BenchServerStream($request);
        $count = 0;
        foreach ($call->responses() as $_reply) {
            $count++;
        }

        self::assertSame(3, $count);
        self::assertSame(\Grpc\STATUS_OK, $call->getStatus()->code, $call->getStatus()->details);
    }

    public function testNativeTlsWithInvalidRootCertFails(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $client = new GreeterClient(self::TLS_TARGET, [
            'credentials' => ChannelCredentials::createSsl("-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n"),
        ]);

        $request = new HelloRequest();
        $request->setName('TLS');
        [$response, $status] = $client->SayHello($request)->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_UNAVAILABLE, $status->code);
        self::assertSame('failed to load root certificates', $status->details);
    }

    public function testNativeTlsCertificateVerificationFailureIncludesDetail(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $rootCerts = file_get_contents(self::CLIENT_CERT_PATH);
        self::assertNotFalse($rootCerts);

        $client = new GreeterClient(self::TLS_TARGET, [
            'credentials' => ChannelCredentials::createSsl($rootCerts),
        ]);

        $request = new HelloRequest();
        $request->setName('TLS verify');
        [$response, $status] = $client->SayHello($request)->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_UNAVAILABLE, $status->code);
        self::assertStringContainsString('TLS certificate verification failed', $status->details);
    }

    public function testNativeTlsAlpnMismatchIncludesDetail(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }
        if (!function_exists('proc_open')) {
            self::markTestSkipped('proc_open is required for the local TLS fixture');
        }

        $serverCert = realpath(__DIR__ . '/../../poc/test-server/certs/server.crt');
        $serverKey = realpath(__DIR__ . '/../../poc/test-server/certs/server.key');
        self::assertIsString($serverCert);
        self::assertIsString($serverKey);

        $serverCode = <<<'PHP'
$serverCert = getenv('GRPC_TEST_SERVER_CERT');
$serverKey = getenv('GRPC_TEST_SERVER_KEY');
$context = stream_context_create([
    'ssl' => [
        'local_cert' => $serverCert,
        'local_pk' => $serverKey,
        'allow_self_signed' => true,
    ],
]);
$server = stream_socket_server('tcp://127.0.0.1:0', $errno, $errstr, STREAM_SERVER_BIND | STREAM_SERVER_LISTEN, $context);
if (!is_resource($server)) {
    fwrite(STDERR, $errstr . PHP_EOL);
    exit(1);
}
echo stream_socket_get_name($server, false), PHP_EOL;
flush();
$conn = @stream_socket_accept($server, 5);
if (is_resource($conn)) {
    @stream_socket_enable_crypto($conn, true, STREAM_CRYPTO_METHOD_TLS_SERVER);
    usleep(100_000);
    fclose($conn);
}
fclose($server);
PHP;
        $process = proc_open(
            [\PHP_BINARY, '-r', $serverCode],
            [
                1 => ['pipe', 'w'],
                2 => ['pipe', 'w'],
            ],
            $pipes,
            null,
            [
                'GRPC_TEST_SERVER_CERT' => $serverCert,
                'GRPC_TEST_SERVER_KEY' => $serverKey,
            ],
        );
        if (!is_resource($process)) {
            self::markTestSkipped('failed to spawn local TLS fixture');
        }
        $address = trim((string) fgets($pipes[1]));
        self::assertNotSame('', $address);

        try {
            $rootCerts = file_get_contents(self::CA_PATH);
            self::assertNotFalse($rootCerts);
            $client = new GreeterClient($address, [
                'credentials' => ChannelCredentials::createSsl($rootCerts),
            ]);

            $request = new HelloRequest();
            $request->setName('TLS ALPN');
            [$response, $status] = $client->SayHello($request, [], ['timeout' => 500_000])->wait();

            self::assertNull($response);
            self::assertSame(\Grpc\STATUS_UNAVAILABLE, $status->code);
            self::assertStringContainsString('TLS ALPN did not negotiate h2', $status->details);
        } finally {
            fclose($pipes[1]);
            fclose($pipes[2]);
            proc_terminate($process);
            proc_close($process);
        }
    }

    public function testNativeTlsHandshakeUsesRpcDeadlineAsUpperBound(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }
        if (!function_exists('proc_open')) {
            self::markTestSkipped('proc_open is required for the local stalled TLS fixture');
        }

        $serverCode = <<<'PHP'
$server = stream_socket_server('tcp://127.0.0.1:0', $errno, $errstr);
if (!is_resource($server)) {
    fwrite(STDERR, $errstr . PHP_EOL);
    exit(1);
}
echo stream_socket_get_name($server, false), PHP_EOL;
flush();
$conn = @stream_socket_accept($server, 5);
if (is_resource($conn)) {
    usleep(500_000);
    fclose($conn);
}
fclose($server);
PHP;
        $process = proc_open(
            [\PHP_BINARY, '-r', $serverCode],
            [
                1 => ['pipe', 'w'],
                2 => ['pipe', 'w'],
            ],
            $pipes,
        );
        if (!is_resource($process)) {
            self::markTestSkipped('failed to spawn local stalled TLS fixture');
        }
        $address = trim((string) fgets($pipes[1]));
        self::assertNotSame('', $address);

        try {
            $rootCerts = file_get_contents(self::CA_PATH);
            self::assertNotFalse($rootCerts);
            $client = new GreeterClient($address, [
                'credentials' => ChannelCredentials::createSsl($rootCerts),
            ]);

            $request = new HelloRequest();
            $request->setName('TLS deadline');
            $started = hrtime(true);
            [$response, $status] = $client->SayHello($request, [], ['timeout' => 50_000])->wait();
            $elapsedMs = (hrtime(true) - $started) / 1_000_000;

            self::assertNull($response);
            self::assertSame(\Grpc\STATUS_DEADLINE_EXCEEDED, $status->code, $status->details);
            self::assertLessThan(400, $elapsedMs);
        } finally {
            fclose($pipes[1]);
            fclose($pipes[2]);
            proc_terminate($process);
            proc_close($process);
        }
    }

    public function testNativeMtlsWithoutClientCertificateFails(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $rootCerts = file_get_contents(self::CA_PATH);
        self::assertNotFalse($rootCerts);

        $client = new GreeterClient(self::MTLS_TARGET, [
            'credentials' => ChannelCredentials::createSsl($rootCerts),
        ]);

        $request = new HelloRequest();
        $request->setName('mTLS');
        [$response, $status] = $client->SayHello($request)->wait();

        self::assertNull($response);
        self::assertNotSame(\Grpc\STATUS_OK, $status->code);
        self::assertNotSame('', $status->details);
    }

    public function testNativeUnaryHonorsMaxReceiveMessageLength(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
            'grpc.max_receive_message_length' => 16,
        ]);

        $request = new BenchRequest();
        $request->setPayloadBytes(100);

        [$response, $status] = $client->BenchUnary($request)->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_RESOURCE_EXHAUSTED, $status->code);
        self::assertSame('received message exceeds maximum size', $status->details);
    }

    public function testNativeServerStreamingHonorsMaxReceiveMessageLength(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
            'grpc.max_receive_message_length' => 16,
        ]);

        $request = new BenchRequest();
        $request->setMessageCount(1);
        $request->setPayloadBytes(100);

        $call = $client->BenchServerStream($request);
        $count = 0;
        foreach ($call->responses() as $_reply) {
            $count++;
        }

        self::assertSame(0, $count);
        self::assertSame(\Grpc\STATUS_RESOURCE_EXHAUSTED, $call->getStatus()->code);
        self::assertSame('received message exceeds maximum size', $call->getStatus()->details);
    }

    public function testNativeUnaryTrailersOnlyErrorReturnsGrpcStatusAndMessage(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        [$response, $status] = $client->BenchUnary(new BenchRequest(), [
            'x-bench-error-code' => [(string) \Grpc\STATUS_INVALID_ARGUMENT],
            'x-bench-error-message' => ['bench error with spaces'],
        ])->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_INVALID_ARGUMENT, $status->code);
        self::assertSame('bench error with spaces', $status->details);
        self::assertSame(['3'], $status->metadata['grpc-status'] ?? null);
    }

    public function testNativeServerStreamingTrailersOnlyErrorReturnsGrpcStatusAndMessage(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        $request = new BenchRequest();
        $request->setMessageCount(10);
        $request->setPayloadBytes(100);

        $call = $client->BenchServerStream($request, [
            'x-bench-error-code' => [(string) \Grpc\STATUS_INVALID_ARGUMENT],
            'x-bench-error-message' => ['bench error with spaces'],
        ]);
        $count = 0;
        foreach ($call->responses() as $_reply) {
            $count++;
        }

        self::assertSame(0, $count);
        self::assertSame(\Grpc\STATUS_INVALID_ARGUMENT, $call->getStatus()->code);
        self::assertSame('bench error with spaces', $call->getStatus()->details);
    }

    public function testNativeBinaryMetadataRoundTripUsesRawPhpValues(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
        $values = ["\x00\x01\xff,a"];

        $call = $client->BenchUnary(new BenchRequest(), [
            'x-bench-echo-bin' => $values,
        ]);
        [, $status] = $call->wait();

        self::assertSame(\Grpc\STATUS_OK, $status->code, $status->details);
        self::assertSame($values, $call->getMetadata()['x-bench-initial-bin'] ?? null);
        self::assertSame($values, $call->getTrailingMetadata()['x-bench-trailing-bin'] ?? null);
    }

    public function testNativeUnaryHttpStatusWithoutGrpcStatusIsMapped(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $client = new GreeterClient('test-server:50054', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        [$response, $status] = $client->BenchUnary(new BenchRequest(), [
            'x-bench-http-status' => ['503'],
        ])->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_UNAVAILABLE, $status->code);
        self::assertSame('HTTP status 503 without grpc-status', $status->details);
    }

    public function testNativeUnaryRejectsNonGrpcContentType(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $client = new GreeterClient('test-server:50054', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        [$response, $status] = $client->BenchUnary(new BenchRequest())->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_UNKNOWN, $status->code);
        self::assertSame('invalid gRPC content-type: text/plain', $status->details);
    }

    public function testNativeUnaryCompressedMessageIsExplicitlyUnsupported(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $client = new GreeterClient('test-server:50054', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        [$response, $status] = $client->BenchUnary(new BenchRequest(), [
            'x-bench-grpc-response' => ['compressed-flag'],
        ])->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_UNIMPLEMENTED, $status->code);
        self::assertSame('compressed gRPC messages are not supported', $status->details);
    }

    public function testNativeUnaryGrpcEncodingIsExplicitlyUnsupported(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $client = new GreeterClient('test-server:50054', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        [$response, $status] = $client->BenchUnary(new BenchRequest(), [
            'x-bench-grpc-encoding' => ['gzip'],
        ])->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_UNIMPLEMENTED, $status->code);
        self::assertSame('unsupported grpc-encoding: gzip', $status->details);
    }

    public function testNativeExtensionMissingFailsAsStatus(): void
    {
        if ((extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is loaded in this process');
        }

        $client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        $request = new HelloRequest();
        $request->setName('NativeMissing');
        [$response, $status] = $client->SayHello($request)->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_UNAVAILABLE, $status->code);
        self::assertSame('grpc native persistent channel API is not loaded', $status->details);
    }

    public function testNativeUnaryCancelBeforeWaitReturnsCancelledStatus(): void
    {
        $client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        $request = new HelloRequest();
        $request->setName('Cancel');
        $call = $client->SayHello($request);
        $call->cancel();

        [$response, $status] = $call->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_CANCELLED, $status->code);
    }

    public function testNativeUnaryDeadlineExceededIsEnforcedClientSide(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        $request = new BenchRequest();
        $request->setServerDelayMs(100);

        $start = hrtime(true);
        [$response, $status] = $client->BenchUnary($request, [], [
            'timeout' => 10_000,
        ])->wait();
        $elapsedMs = (hrtime(true) - $start) / 1_000_000;

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_DEADLINE_EXCEEDED, $status->code, $status->details);
        self::assertLessThan(80.0, $elapsedMs);
    }

    public function testNativeServerStreamingDeadlineExceededIsEnforcedClientSide(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        $request = new BenchRequest();
        $request->setMessageCount(10);
        $request->setPayloadBytes(100);
        $request->setServerDelayMs(50);

        $call = $client->BenchServerStream($request, [], [
            'timeout' => 20_000,
        ]);

        $count = 0;
        foreach ($call->responses() as $_reply) {
            $count++;
        }

        self::assertLessThan(10, $count);
        self::assertSame(\Grpc\STATUS_DEADLINE_EXCEEDED, $call->getStatus()->code);
    }

    public function testNativeServerStreamingCancelDuringIterationReturnsCancelledStatus(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        $request = new BenchRequest();
        $request->setMessageCount(5);
        $request->setPayloadBytes(100);

        $call = $client->BenchServerStream($request);
        $count = 0;
        foreach ($call->responses() as $_reply) {
            $count++;
            $call->cancel();
        }

        self::assertSame(1, $count);
        self::assertSame(\Grpc\STATUS_CANCELLED, $call->getStatus()->code);
    }

    public function testNativeServerStreamingYieldsMessagesIncrementally(): void
    {
        if (!function_exists('grpc_lite_stream_open')) {
            self::markTestSkipped('grpc_native stream API is not loaded in this process');
        }

        $client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        $request = new BenchRequest();
        $request->setMessageCount(3);
        $request->setPayloadBytes(100);
        $request->setServerDelayMs(50);

        $call = $client->BenchServerStream($request);
        $start = hrtime(true);
        $count = 0;
        foreach ($call->responses() as $_reply) {
            $count++;
            if ($count === 1) {
                $call->cancel();
            }
        }
        $elapsedMs = (hrtime(true) - $start) / 1_000_000;

        self::assertSame(1, $count);
        self::assertLessThan(130.0, $elapsedMs);
        self::assertSame(\Grpc\STATUS_CANCELLED, $call->getStatus()->code);
    }

    public function testNativeChannelGoAwayIsNotReusedForNextRpc(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $first = NativeTransport::unarySimple(
            'test-server:50055',
            '/helloworld.Greeter/BenchUnary',
            '',
            [],
        );
        $second = NativeTransport::unarySimple(
            'test-server:50055',
            '/helloworld.Greeter/BenchUnary',
            '',
            [],
        );

        self::assertSame(\Grpc\STATUS_OK, $first['grpc_status']);
        self::assertTrue($first['raw']['channel_draining'] ?? false);
        self::assertSame(\Grpc\STATUS_OK, $second['grpc_status']);
        self::assertTrue($second['raw']['channel_draining'] ?? false);
    }

    public function testNativePersistentChannelSurvivesPhpRequestLocalCacheReset(): void
    {
        if (!function_exists('grpc_lite_unary')) {
            self::markTestSkipped('grpc_native persistent channel API is not loaded in this process');
        }

        $key = 'phpunit-persistent-' . bin2hex(random_bytes(8));
        $request = "\0\0\0\0\0";

        $first = \grpc_lite_unary(
            $key,
            'test-server',
            50051,
            '/helloworld.Greeter/BenchUnary',
            $request,
            [],
        );

        $second = \grpc_lite_unary(
            $key,
            'test-server',
            50051,
            '/helloworld.Greeter/BenchUnary',
            $request,
            [],
        );

        self::assertSame(\Grpc\STATUS_OK, $first['grpc_status']);
        self::assertFalse($first['persistent_reused']);
        self::assertSame(\Grpc\STATUS_OK, $second['grpc_status']);
        self::assertTrue($second['persistent_reused']);
        self::assertSame(0, $second['connect_us']);
    }

    public function testNativePersistentChannelCloseEvictsCachedChannel(): void
    {
        if (!function_exists('grpc_lite_channel_close')) {
            self::markTestSkipped('grpc_lite_channel_close is not loaded in this process');
        }

        $credentials = ChannelCredentials::createInsecure();
        NativeTransport::closeChannel('test-server:50051', $credentials);

        $first = NativeTransport::unarySimple(
            'test-server:50051',
            '/helloworld.Greeter/BenchUnary',
            '',
            [],
            credentials: $credentials,
        );
        $second = NativeTransport::unarySimple(
            'test-server:50051',
            '/helloworld.Greeter/BenchUnary',
            '',
            [],
            credentials: $credentials,
        );
        NativeTransport::closeChannel('test-server:50051', $credentials);
        $third = NativeTransport::unarySimple(
            'test-server:50051',
            '/helloworld.Greeter/BenchUnary',
            '',
            [],
            credentials: $credentials,
        );

        self::assertSame(\Grpc\STATUS_OK, $first['grpc_status']);
        self::assertSame(\Grpc\STATUS_OK, $second['grpc_status']);
        self::assertSame(\Grpc\STATUS_OK, $third['grpc_status']);
        self::assertTrue($second['raw']['persistent_reused'] ?? false);
        self::assertFalse($third['raw']['persistent_reused'] ?? true);
    }

    public function testNativeAuthorityOverrideControlsHttp2Authority(): void
    {
        if (!function_exists('grpc_lite_unary')) {
            self::markTestSkipped('grpc_lite_unary is not loaded in this process');
        }

        $result = NativeTransport::unarySimple(
            'test-server:50054',
            '/helloworld.Greeter/BenchUnary',
            '',
            ['x-bench-observe-authority' => ['1']],
            credentials: ChannelCredentials::createInsecure(),
            authority: 'custom.authority:443',
        );

        self::assertSame(\Grpc\STATUS_OK, $result['grpc_status'], $result['details']);
        self::assertSame(['custom.authority:443'], $result['headers']['x-bench-authority'] ?? null);
    }

    public function testNativeResponseMetadataCapFailsAsResourceExhausted(): void
    {
        if (!function_exists('grpc_lite_unary')) {
            self::markTestSkipped('grpc_lite_unary is not loaded in this process');
        }

        $result = NativeTransport::unarySimple(
            'test-server:50051',
            '/helloworld.Greeter/BenchUnary',
            '',
            [
                'x-bench-response-metadata-count' => ['200'],
                'x-bench-response-metadata-value-bytes' => ['600'],
            ],
            credentials: ChannelCredentials::createInsecure(),
        );

        self::assertSame(\Grpc\STATUS_RESOURCE_EXHAUSTED, $result['grpc_status']);
        self::assertSame('received metadata exceeds maximum size', $result['details']);
        self::assertTrue($result['raw']['metadata_too_large'] ?? false);
    }

    public function testNativeStreamResourceDestructReleasesChannelBusyState(): void
    {
        if (!function_exists('grpc_lite_stream_open')) {
            self::markTestSkipped('grpc_native stream API is not loaded in this process');
        }

        $key = 'phpunit-stream-lifecycle-' . bin2hex(random_bytes(8));
        $request = new BenchRequest();
        $request->setMessageCount(3);
        $request->setPayloadBytes(100);
        $serialized = $request->serializeToString();

        $stream = \grpc_lite_stream_open(
            $key,
            'test-server',
            50051,
            '/helloworld.Greeter/BenchServerStream',
            $serialized,
            [],
        );

        try {
            \grpc_lite_stream_open(
                $key,
                'test-server',
                50051,
                '/helloworld.Greeter/BenchServerStream',
                $serialized,
                [],
            );
            self::fail('second stream on a busy native channel unexpectedly opened');
        } catch (\Throwable $e) {
            self::assertStringContainsString('active stream', $e->getMessage());
        }

        unset($stream);

        $nextStream = \grpc_lite_stream_open(
            $key,
            'test-server',
            50051,
            '/helloworld.Greeter/BenchServerStream',
            $serialized,
            [],
        );
        $next = \grpc_lite_stream_next($nextStream);

        self::assertFalse($next['done']);
        self::assertIsString($next['payload']);
        self::assertTrue(\grpc_lite_stream_cancel($nextStream));
    }

    public function testNativeStreamDeadlineReleasesPersistentChannel(): void
    {
        if (!function_exists('grpc_lite_stream_open')) {
            self::markTestSkipped('grpc_native stream API is not loaded in this process');
        }

        $key = 'phpunit-stream-deadline-' . bin2hex(random_bytes(8));
        $request = new BenchRequest();
        $request->setMessageCount(10);
        $request->setPayloadBytes(100);
        $request->setServerDelayMs(50);
        $serialized = $request->serializeToString();

        $stream = \grpc_lite_stream_open(
            $key,
            'test-server',
            50051,
            '/helloworld.Greeter/BenchServerStream',
            $serialized,
            [],
            20_000,
        );
        $done = null;
        while ($done === null || ($done['done'] ?? false) !== true) {
            $done = \grpc_lite_stream_next($stream);
        }
        self::assertTrue($done['timed_out'] ?? false);

        $nextRequest = new BenchRequest();
        $nextRequest->setMessageCount(1);
        $nextRequest->setPayloadBytes(100);
        $nextStream = \grpc_lite_stream_open(
            $key,
            'test-server',
            50051,
            '/helloworld.Greeter/BenchServerStream',
            $nextRequest->serializeToString(),
            [],
        );
        $next = \grpc_lite_stream_next($nextStream);

        self::assertFalse($next['done']);
        self::assertIsString($next['payload']);
        self::assertTrue(\grpc_lite_stream_cancel($nextStream));
    }

    public function testNativeChannelEofIsDiscardedBeforeNextRpc(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $failed = false;
        for ($attempt = 0; $attempt < 4; $attempt++) {
            try {
                $result = NativeTransport::unarySimple(
                    'test-server:50056',
                    '/helloworld.Greeter/BenchUnary',
                    '',
                    [],
                );
                if ($result['grpc_status'] !== \Grpc\STATUS_OK) {
                    $failed = true;
                    break;
                }
            } catch (\RuntimeException) {
                $failed = true;
                break;
            }
        }

        self::assertTrue($failed, 'EOF fixture did not produce a failed RPC');

        $next = NativeTransport::unarySimple(
            'test-server:50056',
            '/helloworld.Greeter/BenchUnary',
            '',
            [],
        );

        self::assertSame(\Grpc\STATUS_OK, $next['grpc_status']);
        self::assertFalse($next['raw']['channel_dead'] ?? true);
    }

    public function testNativeChannelMidStreamFailureIsDiscardedBeforeNextRpc(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $failed = false;
        try {
            $result = NativeTransport::unarySimple(
                'test-server:50057',
                '/helloworld.Greeter/BenchUnary',
                '',
                [],
            );
            $failed = $result['grpc_status'] !== \Grpc\STATUS_OK
                || ($result['raw']['channel_dead'] ?? false) === true;
        } catch (\RuntimeException) {
            $failed = true;
        }

        self::assertTrue($failed, 'mid-stream failure fixture did not produce a failed RPC');

        $next = NativeTransport::unarySimple(
            'test-server:50051',
            '/helloworld.Greeter/BenchUnary',
            '',
            [],
        );

        self::assertSame(\Grpc\STATUS_OK, $next['grpc_status']);
    }

    public function testNativeExtensionExposesOnlyGrpcLiteProductionBridge(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        self::assertFalse(function_exists('grpc_native_unary'));
        self::assertFalse(function_exists('grpc_native_persistent_channel_unary'));
        self::assertFalse(function_exists('grpc_native_stream_open'));
        self::assertFalse(function_exists('grpc_lite_multiplex_unary'));
        self::assertFalse(function_exists('grpc_lite_bench_unary_batch'));
        self::assertTrue(function_exists('grpc_lite_unary'));
        self::assertTrue(function_exists('grpc_lite_stream_open'));
        self::assertTrue(function_exists('grpc_lite_stream_next'));
        self::assertTrue(function_exists('grpc_lite_stream_cancel'));
        self::assertTrue(function_exists('grpc_lite_channel_close'));
    }
}
