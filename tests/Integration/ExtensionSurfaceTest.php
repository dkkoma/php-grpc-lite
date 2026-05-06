<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Error;
use Grpc\Call;
use Grpc\CallCredentials;
use Grpc\Channel;
use Grpc\ChannelCredentials;
use Grpc\Timeval;
use PHPUnit\Framework\TestCase;

final class ExtensionSurfaceTest extends TestCase
{
    public function testProductionBuildDoesNotExposeDiagnosticFunctions(): void
    {
        self::assertFalse(function_exists('grpc_lite_unary'));
        self::assertFalse(function_exists('grpc_lite_stream_open'));
        self::assertFalse(function_exists('grpc_lite_stream_next'));
        self::assertFalse(function_exists('grpc_lite_stream_cancel'));
        self::assertFalse(function_exists('grpc_lite_channel_close'));
        self::assertFalse(function_exists('grpc_lite_multiplex_unary'));
        self::assertFalse(function_exists('grpc_lite_bench_unary_batch'));
    }

    public function testDefaultRootsPemLifecycleSurface(): void
    {
        ChannelCredentials::invalidateDefaultRootsPem();

        self::assertFalse(ChannelCredentials::isDefaultRootsPemSet());

        ChannelCredentials::setDefaultRootsPem("-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----\n");

        self::assertTrue(ChannelCredentials::isDefaultRootsPemSet());

        ChannelCredentials::invalidateDefaultRootsPem();

        self::assertFalse(ChannelCredentials::isDefaultRootsPemSet());
    }

    public function testChannelCloseAcceptsCredentialAwareCacheKeyShape(): void
    {
        $channel = new Channel('test-server:50051', [
            'credentials' => ChannelCredentials::createSsl('root-cert-fixture'),
            'grpc.default_authority' => 'test-server:50051',
            'grpc.ssl_target_name_override' => 'test-server',
        ]);

        $channel->close();

        self::assertSame('test-server:50051', $channel->getTarget());
    }

    public function testInternalObjectsAreNotCloneable(): void
    {
        $channel = new Channel('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
        $call = new Call($channel, '/helloworld.Greeter/SayHello', Timeval::infFuture());

        foreach ([
            ChannelCredentials::createInsecure(),
            CallCredentials::createFromPlugin(static fn (): array => []),
            Timeval::infFuture(),
            $channel,
            $call,
        ] as $object) {
            try {
                clone $object;
                self::fail(sprintf('%s should not be cloneable', $object::class));
            } catch (Error $error) {
                self::assertStringContainsString('uncloneable', $error->getMessage());
            }
        }
    }
}
