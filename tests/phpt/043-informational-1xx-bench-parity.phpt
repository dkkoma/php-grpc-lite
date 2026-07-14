--TEST--
bench diagnostic mirrors informational phase, status-field, budget, and stream ownership semantics
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

$runBatch = static function (string $control, int $iterations = 1, int $timeoutUs = 0): array {
    return grpc_lite_bench_unary_batch(
        'test-server',
        50071,
        '/helloworld.Greeter/BenchUnary',
        "\0\0\0\0\0",
        $iterations,
        ['x-bench-raw-response' => $control],
        timeout_us: $timeoutUs,
    );
};

$valid = $runBatch('valid-informational-iteration-reset', 2);
grpc_lite_phpt_assert_same(2, $valid['ok'], 'valid repeated-1xx batch ok count');
grpc_lite_phpt_assert_same(0, $valid['failed'], 'valid repeated-1xx batch failed count');

$nonterminalStatus = $runBatch('post-informational-nonterminal-status');
grpc_lite_phpt_assert_same(0, $nonterminalStatus['ok'], 'nonterminal grpc-status batch ok count');
grpc_lite_phpt_assert_same(1, $nonterminalStatus['failed'], 'nonterminal grpc-status batch failed count');

$nonterminalDetails = $runBatch('post-informational-nonterminal-status-details');
grpc_lite_phpt_assert_same(0, $nonterminalDetails['ok'], 'nonterminal grpc-status-details-bin batch ok count');
grpc_lite_phpt_assert_same(1, $nonterminalDetails['failed'], 'nonterminal grpc-status-details-bin batch failed count');

$entryBudget = $runBatch('informational-entry-budget');
grpc_lite_phpt_assert_same(0, $entryBudget['ok'], 'informational entry budget batch ok count');
grpc_lite_phpt_assert_same(1, $entryBudget['failed'], 'informational entry budget batch failed count');
grpc_lite_phpt_assert_same(8, $entryBudget['stream_error_code'], 'informational entry budget RST_STREAM code');

$byteBudget = $runBatch('informational-default-byte-budget');
grpc_lite_phpt_assert_same(0, $byteBudget['ok'], 'informational byte budget batch ok count');
grpc_lite_phpt_assert_same(1, $byteBudget['failed'], 'informational byte budget batch failed count');
grpc_lite_phpt_assert_same(8, $byteBudget['stream_error_code'], 'informational byte budget RST_STREAM code');

$foreignPushedReset = $runBatch('foreign-pushed-stream-protocol-rst', timeoutUs: 2_000_000);
grpc_lite_phpt_assert_same(1, $foreignPushedReset['ok'], 'foreign pushed-stream RST batch ok count');
grpc_lite_phpt_assert_same(0, $foreignPushedReset['failed'], 'foreign pushed-stream RST batch failed count');
grpc_lite_phpt_assert_same(0, $foreignPushedReset['stream_error_code'], 'foreign pushed-stream RST does not alter main stream code');

echo "OK\n";
?>
--EXPECT--
OK
