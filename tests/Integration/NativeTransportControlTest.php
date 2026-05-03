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
        if (!extension_loaded('nghttp2_poc')) {
            self::markTestSkipped('nghttp2_poc is not loaded in this process');
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

    public function testNativeMtlsUnarySucceeds(): void
    {
        if (!extension_loaded('nghttp2_poc')) {
            self::markTestSkipped('nghttp2_poc is not loaded in this process');
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

    public function testNativeExtensionMissingFailsAsStatusWithoutCurlFallback(): void
    {
        if (extension_loaded('nghttp2_poc')) {
            self::markTestSkipped('nghttp2_poc is loaded in this process');
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
        self::assertSame('nghttp2_poc extension is not loaded', $status->details);
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
        if (!extension_loaded('nghttp2_poc')) {
            self::markTestSkipped('nghttp2_poc is not loaded in this process');
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
        if (!extension_loaded('nghttp2_poc')) {
            self::markTestSkipped('nghttp2_poc is not loaded in this process');
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
        if (!extension_loaded('nghttp2_poc')) {
            self::markTestSkipped('nghttp2_poc is not loaded in this process');
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

    public function testNativeChannelGoAwayIsNotReusedForNextRpc(): void
    {
        if (!extension_loaded('nghttp2_poc')) {
            self::markTestSkipped('nghttp2_poc is not loaded in this process');
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

    public function testNativeChannelEofIsDiscardedBeforeNextRpc(): void
    {
        if (!extension_loaded('nghttp2_poc')) {
            self::markTestSkipped('nghttp2_poc is not loaded in this process');
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
        if (!extension_loaded('nghttp2_poc')) {
            self::markTestSkipped('nghttp2_poc is not loaded in this process');
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

    public function testNativePocCanRunConcurrentStreamsOnOneHttp2Session(): void
    {
        if (!extension_loaded('nghttp2_poc')) {
            self::markTestSkipped('nghttp2_poc is not loaded in this process');
        }

        $result = \nghttp2_poc_multiplex_unary(
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
}
