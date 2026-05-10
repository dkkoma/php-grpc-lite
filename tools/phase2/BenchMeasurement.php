<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tools\Phase2;

final class BenchMeasurement
{
    /**
     * @param array<string, mixed> $attributes
     * @param array<string, array{value: int|float|string|bool|null, unit?: string|null}> $metrics
     * @return array{name: string, axis: string, subject: string, attributes: object, metrics: array<string, array{value: int|float|string|bool|null, unit?: string|null}>}
     */
    public static function make(
        string $name,
        string $axis,
        string $subject,
        array $attributes,
        array $metrics,
    ): array {
        return [
            'name' => $name,
            'axis' => $axis,
            'subject' => $subject,
            'attributes' => (object) $attributes,
            'metrics' => $metrics,
        ];
    }
}
