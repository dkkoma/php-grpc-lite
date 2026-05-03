<?php
declare(strict_types=1);

require __DIR__ . '/../../vendor/autoload.php';

use Google\Cloud\Spanner\V1\CreateSessionRequest;
use Google\Cloud\Spanner\V1\DeleteSessionRequest;
use Google\Cloud\Spanner\V1\ExecuteSqlRequest;
use Google\Cloud\Spanner\V1\PartialResultSet;
use Google\Cloud\Spanner\V1\ResultSetMetadata;
use Google\Cloud\Spanner\V1\TypeCode;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\DatabaseAdminGrpcClient;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\InstanceAdminGrpcClient;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\SpannerEnv;
use PhpGrpcLite\Tests\Integration\Fixtures\Spanner\SpannerGrpcClient;

$args = $argv;
array_shift($args);

$output = null;
for ($argIndex = 0; $argIndex < count($args); $argIndex++) {
    $arg = $args[$argIndex];
    if ($arg === '--output') {
        $output = $args[++$argIndex] ?? null;
    } elseif (str_starts_with($arg, '--output=')) {
        $output = substr($arg, strlen('--output='));
    } else {
        fwrite(STDERR, "unexpected argument: $arg\n");
        exit(2);
    }
}

$instanceId = 'phpgrpclite-shape-' . strtolower(bin2hex(random_bytes(3)));
$databaseId = 'shapedb';
$instanceAdmin = new InstanceAdminGrpcClient(SpannerEnv::TARGET, SpannerEnv::options());
$databaseAdmin = new DatabaseAdminGrpcClient(SpannerEnv::TARGET, SpannerEnv::options());
$spanner = new SpannerGrpcClient(SpannerEnv::TARGET, SpannerEnv::options());
$sessionName = '';

$queries = [
    [
        'name' => 'one_row_one_int64',
        'description' => '1 row, 1 INT64 column',
        'sql' => 'SELECT 1 AS n',
    ],
    [
        'name' => 'one_row_spanner_like_10_columns',
        'description' => '1 row, 10 columns: 2 DATE, 2 STRING, and scalar columns',
        'sql' => "SELECT 1 AS id, DATE '2026-05-03' AS created_date, DATE '2026-05-04' AS updated_date, 'alpha' AS first_text, 'beta' AS second_text, TRUE AS active, 42 AS score, 3.14 AS ratio, TIMESTAMP '2026-05-03T00:00:00Z' AS event_ts, b'abc' AS payload",
    ],
    [
        'name' => 'ten_rows_one_int64',
        'description' => '10 rows, 1 INT64 column',
        'sql' => 'SELECT n FROM UNNEST(GENERATE_ARRAY(1, 10)) AS n ORDER BY n',
    ],
    [
        'name' => 'ten_rows_spanner_like_10_columns',
        'description' => '10 rows, 10 columns: 2 DATE, 2 STRING, and scalar columns',
        'sql' => "SELECT n AS id, DATE '2026-05-03' AS created_date, DATE '2026-05-04' AS updated_date, 'alpha' AS first_text, 'beta' AS second_text, TRUE AS active, 42 AS score, 3.14 AS ratio, TIMESTAMP '2026-05-03T00:00:00Z' AS event_ts, b'abc' AS payload FROM UNNEST(GENERATE_ARRAY(1, 10)) AS n ORDER BY n",
    ],
    [
        'name' => 'hundred_rows_spanner_like_10_columns',
        'description' => '100 rows, 10 columns: 2 DATE, 2 STRING, and scalar columns',
        'sql' => "SELECT n AS id, DATE '2026-05-03' AS created_date, DATE '2026-05-04' AS updated_date, 'alpha' AS first_text, 'beta' AS second_text, TRUE AS active, 42 AS score, 3.14 AS ratio, TIMESTAMP '2026-05-03T00:00:00Z' AS event_ts, b'abc' AS payload FROM UNNEST(GENERATE_ARRAY(1, 100)) AS n ORDER BY n",
    ],
    [
        'name' => 'thousand_rows_one_int64',
        'description' => '1000 rows, 1 INT64 column',
        'sql' => 'SELECT n FROM UNNEST(GENERATE_ARRAY(1, 1000)) AS n ORDER BY n',
    ],
    [
        'name' => 'one_row_1k_string',
        'description' => '1 row, 1 STRING column around 1KiB',
        'sql' => "SELECT '" . str_repeat('x', 1024) . "' AS payload",
    ],
    [
        'name' => 'one_row_10k_string',
        'description' => '1 row, 1 STRING column around 10KiB',
        'sql' => "SELECT '" . str_repeat('x', 10240) . "' AS payload",
    ],
];

try {
    $instanceName = SpannerEnv::createInstance($instanceAdmin, $instanceId);
    $databaseName = SpannerEnv::createDatabase($databaseAdmin, $instanceName, $databaseId);

    $sessionReq = new CreateSessionRequest();
    $sessionReq->setDatabase($databaseName);
    [$session, $sessionStatus] = $spanner->CreateSession($sessionReq)->wait();
    if ($sessionStatus->code !== \Grpc\STATUS_OK) {
        throw new RuntimeException('CreateSession failed: ' . $sessionStatus->details);
    }
    $sessionName = $session->getName();

    $results = [];
    foreach ($queries as $query) {
        $results[] = inspectQuery($spanner, $sessionName, $query);
    }

    $document = [
        'suite' => 'spanner-streaming-shape',
        'target' => SpannerEnv::TARGET,
        'database' => $databaseName,
        'generated_at' => gmdate('c'),
        'results' => $results,
    ];

    if ($output !== null && $output !== '') {
        $dir = dirname($output);
        if (!is_dir($dir)) {
            mkdir($dir, 0777, true);
        }
        file_put_contents($output, json_encode($document, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES) . "\n");
    }

    printf("%-34s %8s %8s %8s %10s %12s %12s %8s\n", 'case', 'partials', 'rows', 'values', 'wire_b', 'first_b', 'max_b', 'chunked');
    printf("%'-106s\n", '');
    foreach ($results as $result) {
        printf(
            "%-34s %8d %8.1f %8d %10d %12d %12d %8d\n",
            $result['name'],
            $result['partial_count'],
            $result['estimated_rows'],
            $result['total_values'],
            $result['total_serialized_bytes'],
            $result['partials'][0]['serialized_bytes'] ?? 0,
            $result['max_serialized_bytes'],
            $result['chunked_partial_count'],
        );
    }
    if ($output !== null && $output !== '') {
        echo "JSON: $output\n";
    }
} finally {
    if ($sessionName !== '') {
        $deleteReq = new DeleteSessionRequest();
        $deleteReq->setName($sessionName);
        $spanner->DeleteSession($deleteReq)->wait();
    }
    SpannerEnv::deleteInstance($instanceAdmin, $instanceId);
}

/** @param array{name:string,description:string,sql:string} $query */
function inspectQuery(SpannerGrpcClient $spanner, string $sessionName, array $query): array
{
    $request = new ExecuteSqlRequest();
    $request->setSession($sessionName);
    $request->setSql($query['sql']);

    $call = $spanner->ExecuteStreamingSql($request);
    $partials = [];
    $metadata = null;
    $totalValues = 0;
    $totalSerializedBytes = 0;
    $chunkedPartialCount = 0;
    $maxSerializedBytes = 0;

    foreach ($call->responses() as $index => $partial) {
        \assert($partial instanceof PartialResultSet);
        if ($partial->hasMetadata()) {
            $metadata = summarizeMetadata($partial->getMetadata());
        }

        $valueCount = count(iterator_to_array($partial->getValues()));
        $serializedBytes = strlen($partial->serializeToString());
        $resumeTokenBytes = strlen($partial->getResumeToken());
        $chunked = $partial->getChunkedValue();

        $partials[] = [
            'index' => $index + 1,
            'serialized_bytes' => $serializedBytes,
            'value_count' => $valueCount,
            'has_metadata' => $partial->hasMetadata(),
            'has_stats' => $partial->hasStats(),
            'chunked_value' => $chunked,
            'resume_token_bytes' => $resumeTokenBytes,
            'last' => $partial->getLast(),
        ];

        $totalValues += $valueCount;
        $totalSerializedBytes += $serializedBytes;
        $maxSerializedBytes = max($maxSerializedBytes, $serializedBytes);
        if ($chunked) {
            $chunkedPartialCount++;
        }
    }

    $status = $call->getStatus();
    if ($status->code !== \Grpc\STATUS_OK) {
        throw new RuntimeException("ExecuteStreamingSql failed for {$query['name']}: {$status->details}");
    }

    $columnCount = $metadata['column_count'] ?? 0;
    return [
        'name' => $query['name'],
        'description' => $query['description'],
        'sql' => $query['sql'],
        'partial_count' => count($partials),
        'column_count' => $columnCount,
        'estimated_rows' => $columnCount > 0 ? $totalValues / $columnCount : 0,
        'total_values' => $totalValues,
        'total_serialized_bytes' => $totalSerializedBytes,
        'max_serialized_bytes' => $maxSerializedBytes,
        'chunked_partial_count' => $chunkedPartialCount,
        'metadata' => $metadata,
        'partials' => $partials,
    ];
}

function summarizeMetadata(?ResultSetMetadata $metadata): ?array
{
    if ($metadata === null || !$metadata->hasRowType()) {
        return null;
    }

    $fields = [];
    foreach ($metadata->getRowType()->getFields() as $field) {
        $type = $field->getType();
        $fields[] = [
            'name' => $field->getName(),
            'type' => $type !== null ? typeCodeName($type->getCode()) : 'UNKNOWN',
        ];
    }

    return [
        'column_count' => count($fields),
        'fields' => $fields,
    ];
}

function typeCodeName(int $code): string
{
    return match ($code) {
        TypeCode::BOOL => 'BOOL',
        TypeCode::INT64 => 'INT64',
        TypeCode::FLOAT64 => 'FLOAT64',
        TypeCode::FLOAT32 => 'FLOAT32',
        TypeCode::TIMESTAMP => 'TIMESTAMP',
        TypeCode::DATE => 'DATE',
        TypeCode::STRING => 'STRING',
        TypeCode::BYTES => 'BYTES',
        TypeCode::ARRAY => 'ARRAY',
        TypeCode::STRUCT => 'STRUCT',
        TypeCode::NUMERIC => 'NUMERIC',
        TypeCode::JSON => 'JSON',
        TypeCode::PROTO => 'PROTO',
        TypeCode::ENUM => 'ENUM',
        TypeCode::UUID => 'UUID',
        default => 'UNKNOWN',
    };
}
