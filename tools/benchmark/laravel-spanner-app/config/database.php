<?php

declare(strict_types=1);

use BenchApp\Bench\SpannerTraceLogger;

$target = getenv('SPANNER_EMULATOR_HOST') ?: null;
$minSessions = max(1, (int) (getenv('LARAVEL_SPANNER_MIN_SESSIONS') ?: 1));
$client = [
    'projectId' => getenv('DB_SPANNER_PROJECT_ID') ?: 'test-project',
    'transport' => 'grpc',
    'requestTimeout' => 600,
];
if ($target !== null && $target !== '') {
    $client['apiEndpoint'] = $target;
}
if ((getenv('LARAVEL_SPANNER_TRACE') ?: '') !== '' && getenv('LARAVEL_SPANNER_TRACE') !== '0') {
    $client['transportConfig']['grpc']['logger'] = new SpannerTraceLogger();
}

return [
    'default' => 'spanner',
    'connections' => [
        'spanner' => [
            'driver' => 'spanner',
            'instance' => getenv('DB_SPANNER_INSTANCE_ID') ?: 'laravel-bench-instance',
            'database' => getenv('DB_SPANNER_DATABASE_ID') ?: 'laravel-bench-db',
            'cache_path' => __DIR__ . '/../storage/framework/spanner',
            'client' => $client,
            'session_pool' => [
                'minSessions' => $minSessions,
                'maxSessions' => max(100, $minSessions),
            ],
        ],
    ],
];
