<?php
require __DIR__ . '/vendor/autoload.php';

$iter = (int)($argv[1] ?? 200);

$clientOptions = [
    'projectId' => getenv('GOOGLE_CLOUD_PROJECT'),
];

$bdpProbe = getenv('SPANNER_GRPC_BDP_PROBE');
if ($bdpProbe !== false && $bdpProbe !== '') {
    $clientOptions['transportConfig']['grpc']['stubOpts']['grpc.http2.bdp_probe'] = (int) $bdpProbe;
}

$primaryUserAgent = getenv('SPANNER_GRPC_PRIMARY_USER_AGENT');
if ($primaryUserAgent !== false && $primaryUserAgent !== '') {
    $clientOptions['transportConfig']['grpc']['stubOpts']['grpc.primary_user_agent'] = $primaryUserAgent;
}

$spanner = new Google\Cloud\Spanner\SpannerClient($clientOptions);
$db = $spanner
    ->instance(getenv('DB_SPANNER_INSTANCE'))
    ->database(getenv('DB_SPANNER_DATABASE'));

$db->execute('SELECT 1')->rows()->current();

$times = [];
for ($i = 0; $i < $iter; $i++) {
    $t0 = hrtime(true);
    $db->execute('SELECT 1')->rows()->current();
    $times[] = (hrtime(true) - $t0) / 1e3;
}

sort($times);
$n = count($times);
$mean = array_sum($times) / $n;
$pick = fn(float $p) => $times[(int) min($n - 1, $n * $p)];

printf(
    "case=select1  ext.grpc=%s  ext.spanner=%s  iter=%d  mean=%.0fus  p50=%.0f  p90=%.0f  p99=%.0f  min=%.0f  max=%.0f\n",
    phpversion('grpc') ?: 'n/a',
    (\Composer\InstalledVersions::getVersion('google/cloud-spanner') ?? 'n/a'),
    $n, $mean, $pick(0.5), $pick(0.9), $pick(0.99), $times[0], $times[$n - 1]
);
