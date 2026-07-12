<?php
declare(strict_types=1);

function grpc_lite_phpt_repo_root(): string
{
    return dirname(__DIR__, 2);
}

function grpc_lite_phpt_autoload_path(): string
{
    return grpc_lite_phpt_repo_root() . '/vendor/autoload.php';
}

function grpc_lite_phpt_require_autoload(): void
{
    $autoload = grpc_lite_phpt_autoload_path();
    if (!is_file($autoload)) {
        throw new RuntimeException('Composer autoload is missing. Run composer install before integration PHPT tests.');
    }
    require_once $autoload;
}

function grpc_lite_phpt_assert_true(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

function grpc_lite_phpt_assert_same(mixed $expected, mixed $actual, string $message): void
{
    if ($expected !== $actual) {
        throw new RuntimeException(sprintf(
            "%s: expected %s, got %s",
            $message,
            var_export($expected, true),
            var_export($actual, true),
        ));
    }
}

function grpc_lite_phpt_assert_contains(string $needle, string $haystack, string $message): void
{
    if (stripos($haystack, $needle) === false) {
        throw new RuntimeException(sprintf(
            "%s: expected %s to contain %s",
            $message,
            var_export($haystack, true),
            var_export($needle, true),
        ));
    }
}

function grpc_lite_phpt_expect_throw(callable $callback, ?string $messageContains = null, ?string $expectedClass = null): Throwable
{
    try {
        $callback();
    } catch (Throwable $throwable) {
        if ($expectedClass !== null && !$throwable instanceof $expectedClass) {
            throw new RuntimeException(sprintf(
                'expected exception class %s, got %s: %s',
                $expectedClass,
                $throwable::class,
                $throwable->getMessage(),
            ));
        }
        if ($messageContains !== null) {
            grpc_lite_phpt_assert_contains($messageContains, $throwable->getMessage(), 'exception message');
        }
        return $throwable;
    }

    throw new RuntimeException('expected exception was not thrown');
}

function grpc_lite_phpt_can_connect(string $host, int $port): bool
{
    $socket = @fsockopen($host, $port, $errno, $errstr, 0.2);
    if (!is_resource($socket)) {
        return false;
    }
    fclose($socket);
    return true;
}

function grpc_lite_phpt_test_fault_seam_available(): bool
{
    ob_start();
    phpinfo(INFO_MODULES);
    $info = (string) ob_get_clean();
    return str_contains($info, 'grpc_lite test fault seam');
}

function grpc_lite_phpt_skip_if_test_fault_seam_unavailable(): void
{
    if (!grpc_lite_phpt_test_fault_seam_available()) {
        die('skip extension built without --enable-grpc-test-fault');
    }
}

function grpc_lite_phpt_skip_if_integration_unavailable(array $ports = [50051]): void
{
    if (!is_file(grpc_lite_phpt_autoload_path())) {
        die('skip Composer autoload is not available');
    }
    foreach ($ports as $port) {
        if (!grpc_lite_phpt_can_connect('test-server', $port)) {
            die("skip test-server:$port is not reachable");
        }
    }
}
