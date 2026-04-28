<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tools\Phase2;

final class ResultContract
{
    public const SCHEMA = 'php-grpc-lite.phase2-benchmark.v1';

    /**
     * @param list<array<string, mixed>> $measurements
     * @return array<string, mixed>
     */
    public static function document(
        string $suite,
        string $implementation,
        array $measurements,
        ?string $generatedAt = null,
    ): array {
        return [
            'schema' => self::SCHEMA,
            'generated_at' => $generatedAt ?? gmdate('Y-m-d\TH:i:s\Z'),
            'suite' => $suite,
            'implementation' => $implementation,
            'environment' => self::environment(),
            'measurements' => $measurements,
        ];
    }

    /**
     * @param array<string, mixed> $attributes
     * @param array<string, array{value: int|float|string|bool|null, unit?: string|null}> $metrics
     * @return array<string, mixed>
     */
    public static function measurement(
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

    /**
     * @param array<string, mixed> $document
     */
    public static function validate(array $document): void
    {
        self::requireString($document, 'schema');
        self::requireString($document, 'generated_at');
        self::requireString($document, 'suite');
        self::requireString($document, 'implementation');

        if ($document['schema'] !== self::SCHEMA) {
            throw new \InvalidArgumentException('unsupported schema: ' . $document['schema']);
        }
        if (!isset($document['environment']) || !is_array($document['environment'])) {
            throw new \InvalidArgumentException('environment must be an object');
        }
        if (!isset($document['measurements']) || !is_array($document['measurements'])) {
            throw new \InvalidArgumentException('measurements must be an array');
        }

        foreach ($document['measurements'] as $index => $measurement) {
            if (!is_array($measurement)) {
                throw new \InvalidArgumentException("measurement[$index] must be an object");
            }
            self::requireString($measurement, 'name');
            self::requireString($measurement, 'axis');
            self::requireString($measurement, 'subject');
            if (!array_key_exists('attributes', $measurement)) {
                throw new \InvalidArgumentException("measurement[$index].attributes is required");
            }
            if (!isset($measurement['metrics']) || !is_array($measurement['metrics']) || $measurement['metrics'] === []) {
                throw new \InvalidArgumentException("measurement[$index].metrics must be a non-empty object");
            }

            foreach ($measurement['metrics'] as $metricName => $metric) {
                if (!is_string($metricName) || $metricName === '') {
                    throw new \InvalidArgumentException("measurement[$index] has an invalid metric name");
                }
                if (!is_array($metric) || !array_key_exists('value', $metric)) {
                    throw new \InvalidArgumentException("measurement[$index].metrics.$metricName must contain value");
                }
                if (isset($metric['unit']) && !is_string($metric['unit'])) {
                    throw new \InvalidArgumentException("measurement[$index].metrics.$metricName.unit must be a string");
                }
            }
        }
    }

    /**
     * @param array<string, mixed> $document
     */
    public static function encode(array $document): string
    {
        self::validate($document);

        return json_encode($document, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE) . "\n";
    }

    /**
     * @return array<string, string|bool>
     */
    private static function environment(): array
    {
        return [
            'php_version' => PHP_VERSION,
            'php_sapi' => PHP_SAPI,
            'os' => php_uname('s'),
            'machine' => php_uname('m'),
            'hostname' => gethostname() ?: '',
            'opcache_cli' => filter_var(ini_get('opcache.enable_cli'), FILTER_VALIDATE_BOOL) === true,
            'xdebug_loaded' => extension_loaded('xdebug'),
        ];
    }

    /**
     * @param array<string, mixed> $data
     */
    private static function requireString(array $data, string $key): void
    {
        if (!isset($data[$key]) || !is_string($data[$key]) || $data[$key] === '') {
            throw new \InvalidArgumentException("$key must be a non-empty string");
        }
    }
}
