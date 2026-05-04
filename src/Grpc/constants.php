<?php
declare(strict_types=1);

namespace Grpc;

// Guard against double-definition when the official ext-grpc happens to be
// loaded alongside this package. The native extension wins, our autoloader
// classes stay dormant, and these constant defines become no-ops.
if (!defined(__NAMESPACE__ . '\\STATUS_OK')) {
    define(__NAMESPACE__ . '\\VERSION', '0.1.0-dev');
    define(__NAMESPACE__ . '\\STATUS_OK', 0);
    define(__NAMESPACE__ . '\\STATUS_CANCELLED', 1);
    define(__NAMESPACE__ . '\\STATUS_UNKNOWN', 2);
    define(__NAMESPACE__ . '\\STATUS_INVALID_ARGUMENT', 3);
    define(__NAMESPACE__ . '\\STATUS_DEADLINE_EXCEEDED', 4);
    define(__NAMESPACE__ . '\\STATUS_NOT_FOUND', 5);
    define(__NAMESPACE__ . '\\STATUS_ALREADY_EXISTS', 6);
    define(__NAMESPACE__ . '\\STATUS_PERMISSION_DENIED', 7);
    define(__NAMESPACE__ . '\\STATUS_RESOURCE_EXHAUSTED', 8);
    define(__NAMESPACE__ . '\\STATUS_FAILED_PRECONDITION', 9);
    define(__NAMESPACE__ . '\\STATUS_ABORTED', 10);
    define(__NAMESPACE__ . '\\STATUS_OUT_OF_RANGE', 11);
    define(__NAMESPACE__ . '\\STATUS_UNIMPLEMENTED', 12);
    define(__NAMESPACE__ . '\\STATUS_INTERNAL', 13);
    define(__NAMESPACE__ . '\\STATUS_UNAVAILABLE', 14);
    define(__NAMESPACE__ . '\\STATUS_DATA_LOSS', 15);
    define(__NAMESPACE__ . '\\STATUS_UNAUTHENTICATED', 16);
}
