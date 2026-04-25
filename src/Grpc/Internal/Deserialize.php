<?php
declare(strict_types=1);

namespace Grpc\Internal;

/**
 * Decodes a wire-format payload using a deserialize spec passed by the
 * generated stub. Two shapes are supported, matching ext-grpc semantics:
 *
 *   1. `[ClassName::class, 'decode']`  — gax / protoc-gen-php-grpc style.
 *      `Google\Protobuf\Internal\Message` doesn't expose a static `decode`
 *      method; ext-grpc resolves this by instantiating the class and
 *      invoking `mergeFromString` on it. We mirror that behavior so generated
 *      stubs work unchanged.
 *
 *   2. Any other callable (closure, real static method, etc.) — invoked
 *      directly with the payload bytes.
 *
 * @internal
 */
final class Deserialize
{
    /**
     * @param callable|array{class-string, string} $deserialize
     */
    public static function apply($deserialize, string $bytes): object
    {
        if (
            is_array($deserialize)
            && count($deserialize) === 2
            && is_string($deserialize[0])
            && $deserialize[1] === 'decode'
            && !method_exists($deserialize[0], 'decode')
        ) {
            $message = new $deserialize[0]();
            $message->mergeFromString($bytes);
            return $message;
        }
        return $deserialize($bytes);
    }
}
