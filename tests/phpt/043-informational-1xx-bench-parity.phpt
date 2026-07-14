--TEST--
bench diagnostic rejects post-informational nonterminal grpc-status
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
require __DIR__ . '/helpers.inc';
grpc_lite_phpt_skip_if_integration_unavailable([50071]);
grpc_lite_phpt_skip_if_bench_diagnostics_unavailable();
?>
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';

$result = grpc_lite_bench_unary_batch(
    'test-server',
    50071,
    '/helloworld.Greeter/BenchUnary',
    "\0\0\0\0\0",
    1,
    ['x-bench-raw-response' => 'post-informational-nonterminal-status'],
);

grpc_lite_phpt_assert_same(0, $result['ok'], 'malformed post-1xx batch ok count');
grpc_lite_phpt_assert_same(1, $result['failed'], 'malformed post-1xx batch failed count');

echo "OK\n";
?>
--EXPECT--
OK
