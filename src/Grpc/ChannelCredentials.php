<?php
declare(strict_types=1);

namespace Grpc;

/**
 * Marker / configuration object representing how a channel authenticates to
 * its peer. Constructed only via the static factories — the constructor is
 * private to mirror the ext-grpc surface.
 */
final class ChannelCredentials
{
    public const TYPE_INSECURE = 'insecure';
    public const TYPE_SSL = 'ssl';
    public const TYPE_DEFAULT = 'default';

    private function __construct(
        public readonly string $type,
        public readonly ?string $rootCerts = null,
        public readonly ?string $privateKey = null,
        public readonly ?string $certChain = null,
    ) {}

    public static function createInsecure(): self
    {
        return new self(self::TYPE_INSECURE);
    }

    public static function createSsl(
        ?string $rootCerts = null,
        ?string $privateKey = null,
        ?string $certChain = null,
    ): self {
        return new self(self::TYPE_SSL, $rootCerts, $privateKey, $certChain);
    }

    public static function createDefault(): self
    {
        return new self(self::TYPE_DEFAULT);
    }

    public function isInsecure(): bool
    {
        return $this->type === self::TYPE_INSECURE;
    }
}
