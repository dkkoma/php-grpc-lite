<?php
require __DIR__ . '/vendor/autoload.php';

$iter = (int)($argv[1] ?? 10);
$padBytes = (int) (getenv('SPANNER_GRPC_EXTRA_HEADER_BYTES') ?: 0);
$sql = getenv('SPANNER_GRPC_SELECT_SQL') ?: 'SELECT Id, DateA, DateB, StringA, StringB, IntA, IntB, BoolA, FloatA, StringC FROM BenchRows WHERE Id = 1';

$clientOptions = [
    'projectId' => getenv('GOOGLE_CLOUD_PROJECT'),
];

$primaryUserAgent = getenv('SPANNER_GRPC_PRIMARY_USER_AGENT');
if ($primaryUserAgent !== false && $primaryUserAgent !== '') {
    $clientOptions['transportConfig']['grpc']['stubOpts']['grpc.primary_user_agent'] = $primaryUserAgent;
}

$spanner = new Google\Cloud\Spanner\SpannerClient($clientOptions);
$db = $spanner->instance(getenv('DB_SPANNER_INSTANCE'))->database(getenv('DB_SPANNER_DATABASE'));

$options = [];
if ($padBytes > 0) {
    $options['headers'] = ['x-bench-padding' => [str_repeat('x', $padBytes)]];
}

for ($i = 0; $i < $iter; $i++) {
    $epochStart = microtime(true);
    $monoStartUs = intdiv(hrtime(true), 1000);
    printf("MARK start iter=%d epoch_us=%.0f mono_us=%d pad_bytes=%d\n", $i, $epochStart * 1000000, $monoStartUs, $padBytes);
    flush();

    $rows = $db->execute($sql, $options)->rows();
    $row = $rows->current();
    if ($row === null) {
        throw new RuntimeException('expected one row');
    }

    $monoEndUs = intdiv(hrtime(true), 1000);
    $epochEnd = microtime(true);
    printf("MARK end iter=%d epoch_us=%.0f mono_us=%d elapsed_us=%d pad_bytes=%d\n", $i, $epochEnd * 1000000, $monoEndUs, $monoEndUs - $monoStartUs, $padBytes);
    flush();
}
