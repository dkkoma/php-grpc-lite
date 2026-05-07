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
        self::assertFalse(function_exists('grpc_lite_server_streaming_open'));
        self::assertFalse(function_exists('grpc_lite_server_streaming_next'));
        self::assertFalse(function_exists('grpc_lite_server_streaming_cancel'));
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

    public function testChannelRejectsDoubleConstructionAndUninitializedUse(): void
    {
        $channel = new Channel('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        $this->expectException(\Throwable::class);
        $channel->__construct('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
    }

    public function testUninitializedChannelMethodThrows(): void
    {
        $channel = (new \ReflectionClass(Channel::class))->newInstanceWithoutConstructor();

        $this->expectException(\Throwable::class);
        $channel->getTarget();
    }

    public function testChannelRejectsNulInTarget(): void
    {
        $this->expectException(\Throwable::class);

        new Channel("test-server\0:50051", [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
    }

    public function testChannelCanBeConstructedAfterFailedConstructionAttempt(): void
    {
        $channel = (new \ReflectionClass(Channel::class))->newInstanceWithoutConstructor();

        try {
            $channel->__construct("test-server\0:50051", [
                'credentials' => ChannelCredentials::createInsecure(),
            ]);
            self::fail('expected invalid target to throw');
        } catch (\Throwable) {
        }

        $channel->__construct('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        self::assertSame('test-server:50051', $channel->getTarget());
    }

    public function testChannelRejectsInvalidMetadataLimitOptions(): void
    {
        $this->expectException(\Throwable::class);

        new Channel('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
            'grpc.max_metadata_size' => 1024,
            'grpc.absolute_max_metadata_size' => 512,
        ]);
    }

    public function testHttp2WindowIniEntriesAreRegistered(): void
    {
        self::assertSame('8388608', ini_get('grpc_lite.http2_stream_window_size'));
        self::assertSame('8388608', ini_get('grpc_lite.http2_connection_window_size'));
        self::assertSame('32', ini_get('grpc_lite.server_streaming_read_ahead_max_messages'));
        self::assertSame('8388608', ini_get('grpc_lite.server_streaming_read_ahead_max_bytes'));
    }

    public function testCallRejectsInvalidMethodPathAndUninitializedUse(): void
    {
        $channel = new Channel('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);

        $this->expectException(\Throwable::class);
        new Call($channel, '/bad method/Name', Timeval::infFuture());
    }

    public function testUninitializedCallMethodThrows(): void
    {
        $call = (new \ReflectionClass(Call::class))->newInstanceWithoutConstructor();

        $this->expectException(\Throwable::class);
        $call->getPeer();
    }
}
