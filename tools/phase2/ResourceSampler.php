<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tools\Phase2;

final class ResourceSampler
{
    /**
     * @template T
     * @param callable(): T $subject
     * @return array{result: T, metrics: array<string, array{value: int|float, unit: string}>}
     */
    public static function measure(callable $subject): array
    {
        gc_collect_cycles();

        $usageBefore = getrusage();
        $memoryBefore = memory_get_usage(true);
        $peakBefore = memory_get_peak_usage(true);
        $startNs = hrtime(true);

        $result = $subject();

        $elapsedNs = hrtime(true) - $startNs;
        $peakAfter = memory_get_peak_usage(true);
        $memoryAfter = memory_get_usage(true);
        $usageAfter = getrusage();

        $diagnosticUserCpuUs = self::timevalDeltaUs($usageBefore, $usageAfter, 'ru_utime');
        $diagnosticSystemCpuUs = self::timevalDeltaUs($usageBefore, $usageAfter, 'ru_stime');
        $diagnosticRssMaxBeforeKiB = (int) ($usageBefore['ru_maxrss'] ?? 0);
        $diagnosticRssMaxAfterKiB = (int) ($usageAfter['ru_maxrss'] ?? 0);

        return [
            'result' => $result,
            'metrics' => [
                'wall_time_ns_total' => [
                    'value' => $elapsedNs,
                    'unit' => 'ns',
                ],
                'diagnostic_cpu_user_us_total' => [
                    'value' => $diagnosticUserCpuUs,
                    'unit' => 'us',
                ],
                'diagnostic_cpu_system_us_total' => [
                    'value' => $diagnosticSystemCpuUs,
                    'unit' => 'us',
                ],
                'diagnostic_cpu_total_us_total' => [
                    'value' => $diagnosticUserCpuUs + $diagnosticSystemCpuUs,
                    'unit' => 'us',
                ],
                'diagnostic_rss_max_kib' => [
                    'value' => $diagnosticRssMaxAfterKiB,
                    'unit' => 'KiB',
                ],
                'diagnostic_rss_max_delta_kib' => [
                    'value' => max(0, $diagnosticRssMaxAfterKiB - $diagnosticRssMaxBeforeKiB),
                    'unit' => 'KiB',
                ],
                'memory_usage_delta_bytes' => [
                    'value' => $memoryAfter - $memoryBefore,
                    'unit' => 'bytes',
                ],
                'memory_peak_delta_bytes' => [
                    'value' => max(0, $peakAfter - $peakBefore),
                    'unit' => 'bytes',
                ],
            ],
        ];
    }

    /**
     * @param array<string, mixed> $before
     * @param array<string, mixed> $after
     */
    private static function timevalDeltaUs(array $before, array $after, string $key): int
    {
        $beforeSec = (int) ($before[$key . '.tv_sec'] ?? 0);
        $beforeUsec = (int) ($before[$key . '.tv_usec'] ?? 0);
        $afterSec = (int) ($after[$key . '.tv_sec'] ?? 0);
        $afterUsec = (int) ($after[$key . '.tv_usec'] ?? 0);

        return (($afterSec - $beforeSec) * 1_000_000) + ($afterUsec - $beforeUsec);
    }
}
