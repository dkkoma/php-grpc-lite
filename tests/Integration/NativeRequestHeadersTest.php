<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\AbstractCall;
use Grpc\Channel;
use Grpc\ChannelCredentials;
use PHPUnit\Framework\TestCase;

final class NativeRequestHeadersTest extends TestCase
{
    public function testNativeHeadersCarryTimeoutAndUserAgent(): void
    {
        $headers = $this->call(
            ['grpc.primary_user_agent' => 'review-agent/1.0'],
            ['timeout' => 120_000_000],
        )->nativeRequestHeaders();

        self::assertSame(['review-agent/1.0 php-grpc-lite/' . \Grpc\VERSION], $headers['user-agent'] ?? null);
        self::assertSame(['120000m'], $headers['grpc-timeout'] ?? null);
    }

    public function testNativeHeadersFilterLibraryOwnedMetadata(): void
    {
        $headers = $this->call([], [], [
            'content-type' => ['text/plain'],
            'te' => ['not-trailers'],
            'x-custom' => ['ok'],
        ])->nativeRequestHeaders();

        self::assertArrayNotHasKey('content-type', $headers);
        self::assertArrayNotHasKey('te', $headers);
        self::assertSame(['ok'], $headers['x-custom'] ?? null);
    }

    /** @param array<string, mixed> $options */
    private function call(array $options = [], array $callOptions = [], array $metadata = []): ExposedNativeRequestHeadersCall
    {
        $channel = new Channel('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ] + $options);

        return new ExposedNativeRequestHeadersCall($channel, '/test.Service/Method', null, $metadata, $callOptions);
    }
}

final class ExposedNativeRequestHeadersCall extends AbstractCall
{
    public function nativeRequestHeaders(): array
    {
        return $this->buildNativeRequestHeaders();
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
