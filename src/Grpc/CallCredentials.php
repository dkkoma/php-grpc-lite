<?php
declare(strict_types=1);

namespace Grpc;

/**
 * Thin holder for per-call metadata credentials.
 *
 * ext-grpc exposes CallCredentials::createFromPlugin() as a callback wrapper.
 * php-grpc-lite normalizes it into the same callback shape used internally by
 * gax: callable(string $serviceUrl, string $methodName): array.
 */
final class CallCredentials
{
    /** @param callable(string, string): array<string, string|list<string>> $callback */
    private function __construct(private readonly mixed $callback) {}

    /** @param callable(string, string): array<string, string|list<string>> $callback */
    public static function createFromPlugin(callable $callback): self
    {
        return new self($callback);
    }

    /** @return array<string, string|list<string>> */
    public function getMetadata(string $serviceUrl, string $methodName): array
    {
        $metadata = ($this->callback)($serviceUrl, $methodName);
        if (!is_array($metadata)) {
            throw new \InvalidArgumentException('CallCredentials plugin must return an array');
        }

        return $metadata;
    }
}
