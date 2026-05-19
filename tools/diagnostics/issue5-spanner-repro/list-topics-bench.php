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

$pubsub = new Google\Cloud\PubSub\PubSubClient($clientOptions);

$readFirstTopic = static function () use ($pubsub): void {
    foreach ($pubsub->topics(['pageSize' => 1]) as $_topic) {
        break;
    }
};

$readFirstTopic();

$times = [];
for ($i = 0; $i < $iter; $i++) {
    $t0 = hrtime(true);
    $readFirstTopic();
    $times[] = (hrtime(true) - $t0) / 1e3;
}

sort($times);
$n = count($times);
$mean = array_sum($times) / $n;
$pick = fn(float $p) => $times[(int) min($n - 1, $n * $p)];

printf(
    "case=pubsub-list-topics  ext.grpc=%s  ext.pubsub=%s  iter=%d  mean=%.0fus  p50=%.0f  p90=%.0f  p99=%.0f  min=%.0f  max=%.0f\n",
    phpversion('grpc') ?: 'n/a',
    (\Composer\InstalledVersions::getVersion('google/cloud-pubsub') ?? 'n/a'),
    $n,
    $mean,
    $pick(0.5),
    $pick(0.9),
    $pick(0.99),
    $times[0],
    $times[$n - 1]
);
