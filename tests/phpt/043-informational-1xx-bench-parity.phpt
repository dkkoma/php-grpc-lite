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
        poll_loop: $timeoutUs > 0,
        timeout_us: $timeoutUs,
    );
};

$entryReset = $runBatch('valid-informational-iteration-reset', 2);
grpc_lite_phpt_assert_same(2, $entryReset['ok'], 'entry-counter reset batch ok count');
grpc_lite_phpt_assert_same(0, $entryReset['failed'], 'entry-counter reset batch failed count');

$byteReset = $runBatch('valid-informational-byte-iteration-reset', 2);
grpc_lite_phpt_assert_same(2, $byteReset['ok'], 'byte-counter reset batch ok count');
grpc_lite_phpt_assert_same(0, $byteReset['failed'], 'byte-counter reset batch failed count');

$nonterminalStatus = $runBatch('post-informational-nonterminal-status');
grpc_lite_phpt_assert_same(0, $nonterminalStatus['ok'], 'nonterminal grpc-status batch ok count');
grpc_lite_phpt_assert_same(1, $nonterminalStatus['failed'], 'nonterminal grpc-status batch failed count');

$nonterminalDetails = $runBatch('post-informational-nonterminal-status-details');
grpc_lite_phpt_assert_same(0, $nonterminalDetails['ok'], 'nonterminal grpc-status-details-bin batch ok count');
grpc_lite_phpt_assert_same(1, $nonterminalDetails['failed'], 'nonterminal grpc-status-details-bin batch failed count');

foreach ([
    'post-informational-silent-grpc-status' => 'silent nonterminal grpc-status',
    'post-informational-silent-grpc-message' => 'silent nonterminal grpc-message',
    'post-informational-silent-status-details' => 'silent nonterminal grpc-status-details-bin',
] as $control => $label) {
    $silentStatus = $runBatch($control, timeoutUs: 2_000_000);
    grpc_lite_phpt_assert_same(0, $silentStatus['ok'], "$label batch ok count");
    grpc_lite_phpt_assert_same(1, $silentStatus['failed'], "$label batch failed count");
    grpc_lite_phpt_assert_same(false, $silentStatus['timed_out'], "$label batch is not timeout");
    grpc_lite_phpt_assert_same(8, $silentStatus['stream_error_code'], "$label RST_STREAM code");
}

foreach ([
    'post-informational-incomplete-grpc-status' => 'incomplete grpc-status block',
    'post-informational-incomplete-grpc-message' => 'incomplete grpc-message block',
    'post-informational-incomplete-status-details' => 'incomplete grpc-status-details-bin block',
] as $control => $label) {
    $incompleteStatus = $runBatch($control);
    grpc_lite_phpt_assert_same(0, $incompleteStatus['ok'], "$label batch ok count");
    grpc_lite_phpt_assert_same(1, $incompleteStatus['failed'], "$label batch failed count");
    grpc_lite_phpt_assert_same(false, $incompleteStatus['timed_out'], "$label batch is not timeout");
    grpc_lite_phpt_assert_same(8, $incompleteStatus['stream_error_code'], "$label RST_STREAM code");
}

foreach ([
    ['incomplete-informational-end-stream', 'incomplete 103 END_STREAM block', 1],
    ['incomplete-trailer-without-end-stream', 'incomplete nonterminal trailer block', 1],
    ['post-informational-incomplete-regular-before-status', 'incomplete regular-before-status block', 1],
    ['post-informational-incomplete-invalid-before-status', 'incomplete invalid-regular-before-status block', 1],
    ['post-informational-incomplete-empty-name-before-status', 'incomplete empty-name-before-status block', 1],
    ['post-informational-incomplete-strict-invalid-pseudo-before-status', 'incomplete strict-invalid-pseudo-before-status block', 1],
    ['post-informational-incomplete-uppercase-regular-before-status', 'incomplete uppercase-regular-before-status block', 1],
    ['informational-incomplete-entry-budget', 'incomplete informational entry budget block', 8],
    ['informational-incomplete-invalid-entry-budget', 'incomplete invalid informational entry budget block', 8],
] as [$control, $label, $expectedRstCode]) {
    $incomplete = $runBatch($control);
    grpc_lite_phpt_assert_same(0, $incomplete['ok'], "$label batch ok count");
    grpc_lite_phpt_assert_same(1, $incomplete['failed'], "$label batch failed count");
    grpc_lite_phpt_assert_same(false, $incomplete['timed_out'], "$label batch is not timeout");
    grpc_lite_phpt_assert_same($expectedRstCode, $incomplete['stream_error_code'], "$label RST_STREAM code");
    grpc_lite_phpt_assert_same(true, $incomplete['incomplete_header_fd_nonblocking'], "$label enables nonblocking terminal finish");
    grpc_lite_phpt_assert_true($incomplete['total_us'] < 500_000, "$label batch is finite without poll timeout");
    if ($control === 'post-informational-incomplete-invalid-before-status'
        || $control === 'post-informational-incomplete-empty-name-before-status') {
        grpc_lite_phpt_assert_same(1, $incomplete['invalid_header_callback_count'], "$label invalid-header callback count");
    }
    if ($control === 'post-informational-incomplete-strict-invalid-pseudo-before-status'
        || $control === 'post-informational-incomplete-uppercase-regular-before-status') {
        grpc_lite_phpt_assert_same(0, $incomplete['invalid_header_callback_count'], "$label bypasses invalid-header callback");
    }
    if ($control === 'informational-incomplete-invalid-entry-budget') {
        grpc_lite_phpt_assert_same(128, $incomplete['invalid_header_callback_count'], "$label invalid-header callback TEMPORAL cutoff");
    }
}

$entryBudget = $runBatch('informational-entry-budget', timeoutUs: 2_000_000);
grpc_lite_phpt_assert_same(0, $entryBudget['ok'], 'informational entry budget batch ok count');
grpc_lite_phpt_assert_same(1, $entryBudget['failed'], 'informational entry budget batch failed count');
grpc_lite_phpt_assert_same(false, $entryBudget['timed_out'], 'informational entry budget is not timeout');
grpc_lite_phpt_assert_same(8, $entryBudget['stream_error_code'], 'informational entry budget RST_STREAM code');

$byteBudget = $runBatch('informational-default-byte-budget', timeoutUs: 2_000_000);
grpc_lite_phpt_assert_same(0, $byteBudget['ok'], 'informational byte budget batch ok count');
grpc_lite_phpt_assert_same(1, $byteBudget['failed'], 'informational byte budget batch failed count');
grpc_lite_phpt_assert_same(false, $byteBudget['timed_out'], 'informational byte budget is not timeout');
grpc_lite_phpt_assert_same(8, $byteBudget['stream_error_code'], 'informational byte budget RST_STREAM code');

$invalidEntryBudget = $runBatch('informational-invalid-entry-budget', timeoutUs: 2_000_000);
grpc_lite_phpt_assert_same(0, $invalidEntryBudget['ok'], 'invalid regular entry budget batch ok count');
grpc_lite_phpt_assert_same(1, $invalidEntryBudget['failed'], 'invalid regular entry budget batch failed count');
grpc_lite_phpt_assert_same(false, $invalidEntryBudget['timed_out'], 'invalid regular entry budget is not timeout');
grpc_lite_phpt_assert_same(8, $invalidEntryBudget['stream_error_code'], 'invalid regular entry budget RST_STREAM code');
grpc_lite_phpt_assert_same(128, $invalidEntryBudget['invalid_header_callback_count'], 'diagnostic invalid-header callback TEMPORAL cutoff');

$foreignPushedReset = $runBatch('foreign-pushed-stream-protocol-rst', timeoutUs: 2_000_000);
grpc_lite_phpt_assert_same(1, $foreignPushedReset['ok'], 'foreign pushed-stream RST batch ok count');
grpc_lite_phpt_assert_same(0, $foreignPushedReset['failed'], 'foreign pushed-stream RST batch failed count');
grpc_lite_phpt_assert_same(0, $foreignPushedReset['stream_error_code'], 'foreign pushed-stream RST does not alter main stream code');

echo "OK\n";
?>
--EXPECT--
OK
