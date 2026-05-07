--TEST--
Grpc\Timeval arithmetic saturates and compare does not overflow
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
?>
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';

grpc_lite_phpt_assert_same(123456, (new Grpc\Timeval(123456))->microtime(), 'microtime');
grpc_lite_phpt_assert_same(PHP_INT_MAX, (new Grpc\Timeval(PHP_INT_MAX))->add(new Grpc\Timeval(1))->microtime(), 'add saturates');
grpc_lite_phpt_assert_same(PHP_INT_MIN, (new Grpc\Timeval(PHP_INT_MIN))->subtract(new Grpc\Timeval(1))->microtime(), 'subtract saturates');
grpc_lite_phpt_assert_same(1, (new Grpc\Timeval(PHP_INT_MAX))->compare(new Grpc\Timeval(PHP_INT_MIN)), 'compare max/min');
grpc_lite_phpt_assert_same(-1, (new Grpc\Timeval(PHP_INT_MIN))->compare(new Grpc\Timeval(PHP_INT_MAX)), 'compare min/max');
grpc_lite_phpt_assert_same(0, (new Grpc\Timeval(123))->compare(new Grpc\Timeval(123)), 'compare equal');

echo "OK\n";
?>
--EXPECT--
OK

