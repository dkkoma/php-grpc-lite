<?php

declare(strict_types=1);

namespace GrpcLite\OpenTelemetry;

/**
 * grpc/grpc interceptor that injects the active OpenTelemetry context.
 */
final class TraceContextInterceptor extends \Grpc\Interceptor
{
    public function interceptUnaryUnary(
        $method,
        $argument,
        $deserialize,
        $continuation,
        array $metadata = [],
        array $options = []
    ) {
        return $continuation($method, $argument, $deserialize, TraceContextMetadata::inject($metadata), $options);
    }

    public function interceptUnaryStream(
        $method,
        $argument,
        $deserialize,
        $continuation,
        array $metadata = [],
        array $options = []
    ) {
        return $continuation($method, $argument, $deserialize, TraceContextMetadata::inject($metadata), $options);
    }

    public function interceptStreamUnary(
        $method,
        $deserialize,
        $continuation,
        array $metadata = [],
        array $options = []
    ) {
        return $continuation($method, $deserialize, TraceContextMetadata::inject($metadata), $options);
    }

    public function interceptStreamStream(
        $method,
        $deserialize,
        $continuation,
        array $metadata = [],
        array $options = []
    ) {
        return $continuation($method, $deserialize, TraceContextMetadata::inject($metadata), $options);
    }
}
