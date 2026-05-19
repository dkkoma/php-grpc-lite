<?php
require __DIR__ . '/vendor/autoload.php';

$iter = (int)($argv[1] ?? 200);
$projectId = getenv('GOOGLE_CLOUD_PROJECT');
$secretId = getenv('SECRET_ID') ?: 'test';

$clientOptions = [];

$bdpProbe = getenv('SPANNER_GRPC_BDP_PROBE');
if ($bdpProbe !== false && $bdpProbe !== '') {
    $clientOptions['transportConfig']['grpc']['stubOpts']['grpc.http2.bdp_probe'] = (int) $bdpProbe;
}

$primaryUserAgent = getenv('SPANNER_GRPC_PRIMARY_USER_AGENT');
if ($primaryUserAgent !== false && $primaryUserAgent !== '') {
    $clientOptions['transportConfig']['grpc']['stubOpts']['grpc.primary_user_agent'] = $primaryUserAgent;
}

$client = new Google\Cloud\SecretManager\V1\Client\SecretManagerServiceClient($clientOptions);
$name = $client::secretName($projectId, $secretId);
$request = new Google\Cloud\SecretManager\V1\GetSecretRequest([
    'name' => $name,
]);

$client->getSecret($request);

$times = [];
for ($i = 0; $i < $iter; $i++) {
    $t0 = hrtime(true);
    $client->getSecret($request);
    $times[] = (hrtime(true) - $t0) / 1e3;
}

$client->close();

sort($times);
$n = count($times);
$mean = array_sum($times) / $n;
$pick = fn(float $p) => $times[(int) min($n - 1, $n * $p)];

printf(
    "case=secretmanager-get-secret  secret=%s  ext.grpc=%s  ext.secretmanager=%s  iter=%d  mean=%.0fus  p50=%.0f  p90=%.0f  p99=%.0f  min=%.0f  max=%.0f\n",
    $secretId,
    phpversion('grpc') ?: 'n/a',
    (\Composer\InstalledVersions::getVersion('google/cloud-secret-manager') ?? 'n/a'),
    $n,
    $mean,
    $pick(0.5),
    $pick(0.9),
    $pick(0.99),
    $times[0],
    $times[$n - 1]
);
