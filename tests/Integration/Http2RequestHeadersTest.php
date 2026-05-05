<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\AbstractCall;
use Grpc\Channel;
use Grpc\ChannelCredentials;
use PHPUnit\Framework\TestCase;

final class Http2RequestHeadersTest extends TestCase
{
    public function testHttp2HeadersCarryTimeoutAndUserAgent(): void
    {
        $headers = $this->call(
            ['grpc.primary_user_agent' => 'review-agent/1.0'],
            ['timeout' => 120_000_000],
        )->http2RequestHeaders();

        self::assertSame(['review-agent/1.0 php-grpc-lite/' . \Grpc\VERSION], $headers['user-agent'] ?? null);
        self::assertSame(['120000m'], $headers['grpc-timeout'] ?? null);
    }

    public function testHttp2HeadersFilterLibraryOwnedMetadata(): void
    {
        $headers = $this->call([], [], [
            'content-type' => ['text/plain'],
            'te' => ['not-trailers'],
            'x-custom' => ['ok'],
        ])->http2RequestHeaders();

        self::assertArrayNotHasKey('content-type', $headers);
        self::assertArrayNotHasKey('te', $headers);
        self::assertSame(['ok'], $headers['x-custom'] ?? null);
    }

    /** @param array<string, mixed> $options */
    private function call(array $options = [], array $callOptions = [], array $metadata = []): ExposedHttp2RequestHeadersCall
    {
        $channel = new Channel('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ] + $options);

        return new ExposedHttp2RequestHeadersCall($channel, '/test.Service/Method', null, $metadata, $callOptions);
    }
}

final class ExposedHttp2RequestHeadersCall extends AbstractCall
{
    public function http2RequestHeaders(): array
    {
        return $this->buildHttp2RequestHeaders();
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
