<?php

declare(strict_types=1);

namespace BenchApp\Bench;

use Throwable;

final class SpannerTraceRecorder
{
    /** @var array<string, mixed> */
    private static array $context = [];

    private static int $sequence = 0;

    public static function enabled(): bool
    {
        $value = getenv('LARAVEL_SPANNER_TRACE');
        return $value !== false && $value !== '' && $value !== '0';
    }

    /**
     * @param array<string, mixed> $context
     */
    public static function setContext(array $context): void
    {
        if (!self::enabled()) {
            return;
        }
        self::$context = $context + self::$context;
    }

    /**
     * @param array<string, mixed> $fields
     */
    public static function record(string $event, array $fields = []): void
    {
        if (!self::enabled()) {
            return;
        }

        $record = [
            'time' => self::now(),
            'monotonic_ns' => hrtime(true),
            'pid' => getmypid(),
            'seq' => ++self::$sequence,
            'event' => $event,
        ] + self::$context + $fields;

        $line = json_encode($record, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
        if ($line === false) {
            return;
        }

        $path = self::path();
        $dir = dirname($path);
        if (!is_dir($dir)) {
            @mkdir($dir, 0775, true);
        }

        $handle = @fopen($path, 'ab');
        if ($handle === false) {
            return;
        }
        try {
            @flock($handle, LOCK_EX);
            @fwrite($handle, $line . PHP_EOL);
            @flock($handle, LOCK_UN);
        } finally {
            @fclose($handle);
        }
    }

    /**
     * @template T
     * @param array<string, mixed> $fields
     * @param callable():T $callback
     * @return T
     */
    public static function measure(string $step, array $fields, callable $callback): mixed
    {
        if (!self::enabled()) {
            return $callback();
        }

        $start = hrtime(true);
        self::record('step.start', ['step' => $step] + $fields);
        try {
            $result = $callback();
            self::record('step.end', [
                'step' => $step,
                'elapsed_us' => intdiv(hrtime(true) - $start, 1000),
                'ok' => true,
            ] + $fields);
            return $result;
        } catch (Throwable $throwable) {
            self::record('step.end', [
                'step' => $step,
                'elapsed_us' => intdiv(hrtime(true) - $start, 1000),
                'ok' => false,
                'error_class' => $throwable::class,
                'error_message' => $throwable->getMessage(),
            ] + $fields);
            throw $throwable;
        }
    }

    private static function path(): string
    {
        $path = getenv('LARAVEL_SPANNER_TRACE_FILE');
        if ($path !== false && $path !== '') {
            return $path;
        }
        return dirname(__DIR__, 2) . '/storage/logs/spanner-trace.ndjson';
    }

    private static function now(): string
    {
        $microtime = microtime(true);
        $seconds = (int) $microtime;
        $micros = (int) (($microtime - $seconds) * 1_000_000);
        return gmdate('Y-m-d\\TH:i:s', $seconds) . sprintf('.%06dZ', $micros);
    }
}
