<?php

declare(strict_types=1);

namespace GrpcLite\OpenTelemetry;

/**
 * Injects the active OpenTelemetry context into gRPC metadata.
 *
 * The class intentionally keeps OpenTelemetry as an optional dependency. When
 * the OpenTelemetry API is not installed, metadata is returned unchanged.
 */
final class TraceContextMetadata
{
    /**
     * @param array<string, list<string>|string> $metadata
     * @return array<string, list<string>|string>
     */
    public static function inject(array $metadata): array
    {
        $carrier = [];
        self::injectCarrier($carrier);

        foreach ($carrier as $name => $value) {
            if (!is_string($name) || !is_string($value) || $value === '') {
                continue;
            }
            $metadata[strtolower($name)] = [$value];
        }

        return $metadata;
    }

    /**
     * Returns a callback compatible with grpc/grpc BaseStub's update_metadata option.
     *
     * @return callable(array<string, list<string>|string>, string|null=): array<string, list<string>|string>
     */
    public static function updateMetadataCallback(): callable
    {
        return static fn(array $metadata, ?string $_jwtAudienceUri = null): array => self::inject($metadata);
    }

    /**
     * @param array<string, string> $carrier
     */
    private static function injectCarrier(array &$carrier): void
    {
        if (class_exists(\OpenTelemetry\API\Globals::class) && method_exists(\OpenTelemetry\API\Globals::class, 'propagator')) {
            $propagator = \OpenTelemetry\API\Globals::propagator();
            if (is_object($propagator) && method_exists($propagator, 'inject')) {
                $propagator->inject($carrier);
                return;
            }
        }

        if (class_exists(\OpenTelemetry\API\Trace\Propagation\TraceContextPropagator::class)) {
            $propagator = \OpenTelemetry\API\Trace\Propagation\TraceContextPropagator::getInstance();
            $propagator->inject($carrier);
        }
    }
}
