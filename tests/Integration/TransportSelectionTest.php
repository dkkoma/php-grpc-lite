<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\AbstractCall;
use Grpc\Channel;
use Grpc\ChannelCredentials;
use PHPUnit\Framework\TestCase;

final class TransportSelectionTest extends TestCase
{
    public function testDefaultTransportIsNativeWhenEnvIsUnset(): void
    {
        $this->withTransportEnv(null, function (): void {
            self::assertTrue($this->call()->usesNativeTransport());
        });
    }

    public function testCurlTransportCanBeSelectedByEnv(): void
    {
        $this->withTransportEnv('curl', function (): void {
            self::assertFalse($this->call()->usesNativeTransport());
        });
    }

    public function testExplicitOptionOverridesEnv(): void
    {
        $this->withTransportEnv('curl', function (): void {
            self::assertTrue($this->call(['php_grpc_lite.transport' => 'native'])->usesNativeTransport());
        });
    }

    public function testLegacyNativeTransportOptionStillWorks(): void
    {
        $this->withTransportEnv(null, function (): void {
            self::assertTrue($this->call(['php_grpc_lite.native_transport' => true])->usesNativeTransport());
        });
    }

    public function testInvalidTransportOptionFailsExplicitly(): void
    {
        $this->expectException(\InvalidArgumentException::class);
        $this->call(['php_grpc_lite.transport' => 'auto'])->usesNativeTransport();
    }

    public function testInvalidTransportEnvFailsExplicitly(): void
    {
        $this->withTransportEnv('auto', function (): void {
            $this->expectException(\InvalidArgumentException::class);
            $this->call()->usesNativeTransport();
        });
    }

    public function testGrpcTimeoutHeaderUsesLargestSafeUnit(): void
    {
        $headers = $this->call([], ['timeout' => 120_000_000])->requestHeaders();

        self::assertContains('grpc-timeout: 120000m', $headers);
    }

    /** @param array<string, mixed> $options */
    private function call(array $options = [], array $callOptions = []): ExposedTransportSelectionCall
    {
        $channel = new Channel('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ] + $options);

        return new ExposedTransportSelectionCall($channel, '/test.Service/Method', null, [], $callOptions);
    }

    private function withTransportEnv(?string $value, callable $callback): void
    {
        $previous = getenv('PHP_GRPC_LITE_TRANSPORT');
        if ($value === null) {
            putenv('PHP_GRPC_LITE_TRANSPORT');
        } else {
            putenv("PHP_GRPC_LITE_TRANSPORT=$value");
        }

        try {
            $callback();
        } finally {
            if (is_string($previous)) {
                putenv("PHP_GRPC_LITE_TRANSPORT=$previous");
            } else {
                putenv('PHP_GRPC_LITE_TRANSPORT');
            }
        }
    }
}

final class ExposedTransportSelectionCall extends AbstractCall
{
    public function usesNativeTransport(): bool
    {
        return $this->shouldUseNativeTransport();
    }

    public function requestHeaders(): array
    {
        return $this->buildRequestHeaders();
    }

    public function cancel(): void
    {
    }

    public function getMetadata(): array
    {
        return [];
    }

    public function getTrailingMetadata(): array
    {
        return [];
    }
}
