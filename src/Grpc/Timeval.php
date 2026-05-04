<?php
declare(strict_types=1);

namespace Grpc;

/**
 * Thin value object preserved for compatibility with legacy ext-grpc user
 * code that constructs Timeval directly. The gax integration path uses
 * `'timeout' => int microseconds` in call options and never touches this
 * class.
 */
final class Timeval
{
    public function __construct(public readonly int $microseconds) {}

    public static function infFuture(): self
    {
        return new self(PHP_INT_MAX);
    }

    public static function infPast(): self
    {
        return new self(PHP_INT_MIN);
    }

    public static function now(): self
    {
        return new self((int) (microtime(true) * 1_000_000));
    }

    public function microtime(): int
    {
        return $this->microseconds;
    }

    public function add(Timeval $other): self
    {
        return new self($this->microseconds + $other->microseconds);
    }

    public function subtract(Timeval $other): self
    {
        return new self($this->microseconds - $other->microseconds);
    }

    public function compare(Timeval $other): int
    {
        return $this->microseconds <=> $other->microseconds;
    }
}
