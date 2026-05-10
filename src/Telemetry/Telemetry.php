<?php

declare(strict_types=1);

namespace GrpcLite\Telemetry;

final class Telemetry
{
    /**
     * @param null|callable(array<string, mixed>): void $handler
     */
    public static function setHandler(?callable $handler): void
    {
        if (!\function_exists('grpc_lite_set_telemetry_handler')) {
            return;
        }

        \ini_set('grpc_lite.telemetry_enabled', $handler === null ? '0' : '1');
        \grpc_lite_set_telemetry_handler($handler);
    }
}
