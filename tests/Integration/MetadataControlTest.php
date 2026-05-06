<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\ChannelCredentials;
use Helloworld\BenchRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\TestCase;

#[Group('integration')]
final class MetadataControlTest extends TestCase
{
    public function testFiltersUserSuppliedUserAgentMetadata(): void
    {
        $seen = $this->serverSeen('user-agent', [
            'x-bench-observe-metadata-key' => ['user-agent'],
            'user-agent' => ['user-agent-override'],
        ]);

        self::assertNotContains('user-agent-override', $seen);
    }

    public function testPreservesEmptyAsciiMetadataValue(): void
    {
        $seen = $this->serverSeen('x-bench-empty', [
            'x-bench-observe-metadata-key' => ['x-bench-empty'],
            'x-bench-empty' => [''],
        ]);

        self::assertSame([''], $seen);
    }

    public function testUppercaseMetadataKeyIsNormalizedToLowercase(): void
    {
        $seen = $this->serverSeen('x-bench-upper', [
            'x-bench-observe-metadata-key' => ['x-bench-upper'],
            'X-Bench-Upper' => ['upper'],
        ]);

        self::assertSame(['upper'], $seen);
    }

    public function testInvalidPseudoMetadataKeyIsRejected(): void
    {
        $this->assertInvalidMetadata([
            ':path' => ['/evil.Service/Method'],
        ]);
    }

    public function testInvalidMetadataKeyCharacterIsRejected(): void
    {
        $this->assertInvalidMetadata([
            'x bench space' => ['space-key'],
        ]);
    }

    public function testReservedGrpcMetadataKeyIsRejected(): void
    {
        $this->assertInvalidMetadata([
            'grpc-foo' => ['reserved'],
        ]);
    }

    public function testInvalidAsciiMetadataValueIsRejected(): void
    {
        $this->assertInvalidMetadata([
            'x-bench-crlf' => ["line\r\nbreak"],
        ]);
    }

    public function testInvalidNonAsciiMetadataValueIsRejected(): void
    {
        $this->assertInvalidMetadata([
            'x-bench-utf8' => ['utf8-あ'],
        ]);
    }

    public function testBinaryMetadataValueAllowsRawBytes(): void
    {
        $seen = $this->serverSeen('x-bench-raw-bin', [
            'x-bench-observe-metadata-key' => ['x-bench-raw-bin'],
            'x-bench-raw-bin' => ["\x00\x01\xff"],
        ]);

        self::assertSame(["\x00\x01\xff"], $seen);
    }

    /**
     * @param array<string, string|list<string>> $metadata
     * @return list<string>
     */
    private function serverSeen(string $observedKey, array $metadata): array
    {
        $client = $this->client();
        $call = $client->BenchUnary(new BenchRequest(), $metadata);
        [, $status] = $call->wait();

        self::assertSame(\Grpc\STATUS_OK, $status->code, $status->details);

        $responseMetadata = $call->getMetadata();
        self::assertSame($observedKey, $responseMetadata['x-bench-seen-000-key-bin'][0] ?? null);
        $count = (int) ($responseMetadata['x-bench-seen-000-count'][0] ?? 0);
        $values = [];
        for ($index = 0; $index < $count; $index++) {
            $valueKey = sprintf('x-bench-seen-000-value-%03d-bin', $index);
            $values[] = $responseMetadata[$valueKey][0] ?? null;
        }

        return $values;
    }

    /** @param array<string, string|list<string>> $metadata */
    private function assertInvalidMetadata(array $metadata): void
    {
        try {
            $client = $this->client();
            $call = $client->BenchUnary(new BenchRequest(), $metadata);
            $call->wait();
            self::fail('expected InvalidArgumentException');
        } catch (\InvalidArgumentException $e) {
            self::assertStringContainsString('metadata', $e->getMessage());
        }
    }

    private function client(): GreeterClient
    {
        return new GreeterClient('test-server:50051', [
            'credentials' => ChannelCredentials::createInsecure(),
        ]);
    }
}
