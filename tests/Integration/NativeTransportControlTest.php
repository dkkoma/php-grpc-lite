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
            'php_grpc_lite.transport' => 'native',
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
            'php_grpc_lite.transport' => 'native',
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
            'php_grpc_lite.transport' => 'native',
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
            'php_grpc_lite.transport' => 'native',
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

    public function testNativeTlsWithInvalidRootCertFailsWithoutCurlFallback(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $client = new GreeterClient(self::TLS_TARGET, [
            'credentials' => ChannelCredentials::createSsl("-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n"),
            'php_grpc_lite.transport' => 'native',
        ]);

        $request = new HelloRequest();
        $request->setName('TLS');
        [$response, $status] = $client->SayHello($request)->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_UNAVAILABLE, $status->code);
        self::assertSame('failed to load root certificates', $status->details);
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
                'php_grpc_lite.transport' => 'native',
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

    public function testNativeMtlsWithoutClientCertificateFailsWithoutCurlFallback(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $rootCerts = file_get_contents(self::CA_PATH);
        self::assertNotFalse($rootCerts);

        $client = new GreeterClient(self::MTLS_TARGET, [
            'credentials' => ChannelCredentials::createSsl($rootCerts),
            'php_grpc_lite.transport' => 'native',
        ]);

        $request = new HelloRequest();
        $request->setName('mTLS');
        [$response, $status] = $client->SayHello($request)->wait();

        self::assertNull($response);
        self::assertNotSame(\Grpc\STATUS_OK, $status->code);
    }

    public function testNativeUnaryTrailersOnlyErrorReturnsGrpcStatusAndMessage(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
            'php_grpc_lite.transport' => 'native',
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
            'php_grpc_lite.transport' => 'native',
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
            'php_grpc_lite.transport' => 'native',
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
            'php_grpc_lite.transport' => 'native',
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
            'php_grpc_lite.transport' => 'native',
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
            'php_grpc_lite.transport' => 'native',
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
            'php_grpc_lite.transport' => 'native',
        ]);

        [$response, $status] = $client->BenchUnary(new BenchRequest(), [
            'x-bench-grpc-encoding' => ['gzip'],
        ])->wait();

        self::assertNull($response);
        self::assertSame(\Grpc\STATUS_UNIMPLEMENTED, $status->code);
        self::assertSame('unsupported grpc-encoding: gzip', $status->details);
    }

    public function testNativeExtensionMissingFailsAsStatusWithoutCurlFallback(): void
    {
        if ((extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is loaded in this process');
        }

        $client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
            'php_grpc_lite.transport' => 'native',
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
            'php_grpc_lite.transport' => 'native',
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
            'php_grpc_lite.transport' => 'native',
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
            'php_grpc_lite.transport' => 'native',
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
            'php_grpc_lite.transport' => 'native',
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
        if (!function_exists('grpc_native_stream_open')) {
            self::markTestSkipped('grpc_native stream API is not loaded in this process');
        }

        $client = new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
            'php_grpc_lite.transport' => 'native',
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
        if (!function_exists('grpc_native_persistent_channel_unary')) {
            self::markTestSkipped('grpc_native persistent channel API is not loaded in this process');
        }

        $key = 'phpunit-persistent-' . bin2hex(random_bytes(8));
        $request = "\0\0\0\0\0";

        $first = \grpc_native_persistent_channel_unary(
            $key,
            'test-server',
            50051,
            '/helloworld.Greeter/BenchUnary',
            $request,
            [],
        );

        $second = \grpc_native_persistent_channel_unary(
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

    public function testNativeStreamResourceDestructReleasesChannelBusyState(): void
    {
        if (!function_exists('grpc_native_stream_open')) {
            self::markTestSkipped('grpc_native stream API is not loaded in this process');
        }

        $key = 'phpunit-stream-lifecycle-' . bin2hex(random_bytes(8));
        $request = new BenchRequest();
        $request->setMessageCount(3);
        $request->setPayloadBytes(100);
        $serialized = $request->serializeToString();

        $stream = \grpc_native_stream_open(
            $key,
            'test-server',
            50051,
            '/helloworld.Greeter/BenchServerStream',
            $serialized,
            [],
        );

        try {
            \grpc_native_stream_open(
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

        $nextStream = \grpc_native_stream_open(
            $key,
            'test-server',
            50051,
            '/helloworld.Greeter/BenchServerStream',
            $serialized,
            [],
        );
        $next = \grpc_native_stream_next($nextStream);

        self::assertFalse($next['done']);
        self::assertIsString($next['payload']);
        self::assertTrue(\grpc_native_stream_cancel($nextStream));
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

    public function testNativeBenchCanRunConcurrentStreamsOnOneHttp2Session(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        $result = \grpc_native_multiplex_unary(
            'test-server',
            50051,
            '/helloworld.Greeter/BenchUnary',
            "\0\0\0\0\0",
            8,
        );

        self::assertTrue($result['ok']);
        self::assertSame(8, $result['streams']);
        self::assertSame(8, $result['closed']);
        self::assertSame(array_fill(0, 8, \Grpc\STATUS_OK), $result['grpc_statuses']);
    }

    public function testNativeExtensionDoesNotExposeLegacyOneShotUnaryDiagnostic(): void
    {
        if (!(extension_loaded('grpc'))) {
            self::markTestSkipped('grpc native extension is not loaded in this process');
        }

        self::assertFalse(function_exists('grpc_native_unary'));
        self::assertTrue(function_exists('grpc_native_persistent_channel_unary'));
        self::assertTrue(function_exists('grpc_native_stream_open'));
        self::assertTrue(function_exists('grpc_native_bench_unary_batch'));
    }
}
