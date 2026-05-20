<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tools\Benchmark;

final class RpcGap
{
    private const MAX_GAP_MS = 60_000;

    public static function fromEnvironment(): int
    {
        $value = getenv('BENCH_RPC_GAP_MS');
        if ($value === false || $value === '') {
            return 0;
        }

        return self::parse($value);
    }

    /**
     * @param list<string> $args
     */
    public static function consumeArgument(string $arg, array $args, int &$argIndex, int &$rpcGapMs): bool
    {
        if ($arg === '--rpc-gap-ms') {
            $rpcGapMs = self::parse($args[++$argIndex] ?? '');
            return true;
        }
        if (str_starts_with($arg, '--rpc-gap-ms=')) {
            $rpcGapMs = self::parse(substr($arg, strlen('--rpc-gap-ms=')));
            return true;
        }

        return false;
    }

    public static function sleepBetweenCalls(int $rpcGapMs, bool $hasNextCall): void
    {
        if (!$hasNextCall || $rpcGapMs <= 0) {
            return;
        }

        usleep($rpcGapMs * 1000);
    }

    private static function parse(string $value): int
    {
        if ($value === '' || !preg_match('/^\d+$/', $value)) {
            throw new \InvalidArgumentException('rpc-gap-ms must be a non-negative integer');
        }

        $gapMs = (int) $value;
        if ($gapMs > self::MAX_GAP_MS) {
            throw new \InvalidArgumentException('rpc-gap-ms must be <= 60000');
        }

        return $gapMs;
    }
}
