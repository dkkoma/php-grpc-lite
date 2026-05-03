<?php
declare(strict_types=1);

require __DIR__ . '/../../vendor/autoload.php';

use Google\Cloud\Spanner\V1\CreateSessionRequest;
use Google\Cloud\Spanner\V1\BeginTransactionRequest;
use Google\Cloud\Spanner\V1\CommitRequest;
use Google\Cloud\Spanner\V1\CommitResponse;
use Google\Cloud\Spanner\V1\DeleteSessionRequest;
use Google\Cloud\Spanner\V1\ExecuteSqlRequest;
use Google\Cloud\Spanner\V1\PartialResultSet;
use Google\Cloud\Spanner\V1\ResultSetMetadata;
use Google\Cloud\Spanner\V1\TransactionOptions;
use Google\Cloud\Spanner\V1\TransactionOptions\ReadWrite;
use Google\Cloud\Spanner\V1\TransactionSelector;
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
    $databaseName = SpannerEnv::createDatabase($databaseAdmin, $instanceName, $databaseId, [
        'CREATE TABLE ShapeRows (
            id INT64 NOT NULL,
            created_date DATE,
            updated_date DATE,
            first_text STRING(MAX),
            second_text STRING(MAX),
            active BOOL,
            score INT64,
            ratio FLOAT64,
            event_ts TIMESTAMP,
            payload BYTES(MAX),
        ) PRIMARY KEY (id)',
    ]);

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
    $dmlResults = inspectDmlFlow($spanner, $sessionName);

    $document = [
        'suite' => 'spanner-streaming-shape',
        'target' => SpannerEnv::TARGET,
        'database' => $databaseName,
        'generated_at' => gmdate('c'),
        'results' => $results,
        'dml_results' => $dmlResults,
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
    echo "\n";
    printf("%-12s %-18s %10s %10s %8s\n", 'operation', 'rpc', 'request_b', 'response_b', 'rows');
    printf("%'-64s\n", '');
    foreach ($dmlResults as $operation) {
        foreach ($operation['rpc_shapes'] as $shape) {
            printf(
                "%-12s %-18s %10d %10d %8s\n",
                $operation['operation'],
                $shape['rpc'],
                $shape['request_serialized_bytes'],
                $shape['response_serialized_bytes'],
                $shape['row_count_exact'] ?? '',
            );
        }
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

/** @return list<array<string, mixed>> */
function inspectDmlFlow(SpannerGrpcClient $spanner, string $sessionName): array
{
    $operations = [
        [
            'operation' => 'insert',
            'sql' => "INSERT INTO ShapeRows (id, created_date, updated_date, first_text, second_text, active, score, ratio, event_ts, payload) VALUES (1, DATE '2026-05-03', DATE '2026-05-04', 'alpha', 'beta', TRUE, 42, 3.14, TIMESTAMP '2026-05-03T00:00:00Z', b'abc')",
        ],
        [
            'operation' => 'update',
            'sql' => "UPDATE ShapeRows SET updated_date = DATE '2026-05-05', first_text = 'gamma', second_text = 'delta', active = FALSE, score = 43, ratio = 6.28, event_ts = TIMESTAMP '2026-05-03T01:00:00Z', payload = b'def' WHERE id = 1",
        ],
        [
            'operation' => 'delete',
            'sql' => 'DELETE FROM ShapeRows WHERE id = 1',
        ],
    ];

    $results = [];
    foreach ($operations as $operation) {
        $rpcShapes = [];

        $beginRequest = new BeginTransactionRequest();
        $beginRequest->setSession($sessionName);
        $beginRequest->setOptions(readWriteOptions());
        [$transaction, $beginStatus] = $spanner->BeginTransaction($beginRequest)->wait();
        if ($beginStatus->code !== \Grpc\STATUS_OK) {
            throw new RuntimeException("BeginTransaction failed for {$operation['operation']}: {$beginStatus->details}");
        }
        $transactionId = $transaction->getId();
        $rpcShapes[] = [
            'rpc' => 'BeginTransaction',
            'request_serialized_bytes' => strlen($beginRequest->serializeToString()),
            'response_serialized_bytes' => strlen($transaction->serializeToString()),
            'transaction_id_bytes' => strlen($transactionId),
        ];

        $dmlRequest = new ExecuteSqlRequest();
        $dmlRequest->setSession($sessionName);
        $dmlRequest->setSql($operation['sql']);
        $selector = new TransactionSelector();
        $selector->setId($transactionId);
        $dmlRequest->setTransaction($selector);
        [$resultSet, $dmlStatus] = $spanner->ExecuteSql($dmlRequest)->wait();
        if ($dmlStatus->code !== \Grpc\STATUS_OK) {
            throw new RuntimeException("ExecuteSql DML failed for {$operation['operation']}: {$dmlStatus->details}");
        }
        $stats = $resultSet->getStats();
        $rpcShapes[] = [
            'rpc' => 'ExecuteSqlDml',
            'request_serialized_bytes' => strlen($dmlRequest->serializeToString()),
            'response_serialized_bytes' => strlen($resultSet->serializeToString()),
            'row_count_exact' => $stats !== null ? (int) $stats->getRowCountExact() : null,
            'sql_bytes' => strlen($operation['sql']),
        ];

        $commitRequest = new CommitRequest();
        $commitRequest->setSession($sessionName);
        $commitRequest->setTransactionId($transactionId);
        [$commitResponse, $commitStatus] = $spanner->Commit($commitRequest)->wait();
        if ($commitStatus->code !== \Grpc\STATUS_OK) {
            throw new RuntimeException("Commit failed for {$operation['operation']}: {$commitStatus->details}");
        }
        \assert($commitResponse instanceof CommitResponse);
        $rpcShapes[] = [
            'rpc' => 'Commit',
            'request_serialized_bytes' => strlen($commitRequest->serializeToString()),
            'response_serialized_bytes' => strlen($commitResponse->serializeToString()),
            'transaction_id_bytes' => strlen($transactionId),
        ];

        $results[] = [
            'operation' => $operation['operation'],
            'sql' => $operation['sql'],
            'rpc_shapes' => $rpcShapes,
        ];
    }

    return $results;
}

function readWriteOptions(): TransactionOptions
{
    $options = new TransactionOptions();
    $options->setReadWrite(new ReadWrite());
    return $options;
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
