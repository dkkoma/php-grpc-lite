<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration\Fixtures\Spanner;

use Google\Cloud\Spanner\Admin\Database\V1\CreateDatabaseRequest;
use Google\Cloud\Spanner\Admin\Database\V1\DropDatabaseRequest;
use Google\Cloud\Spanner\Admin\Database\V1\ListDatabasesRequest;
use Google\Cloud\Spanner\Admin\Database\V1\ListDatabasesResponse;
use Google\LongRunning\Operation;
use Google\Protobuf\GPBEmpty;
use Grpc\BaseStub;
use Grpc\UnaryCall;

/**
 * protoc-gen-php-grpc-equivalent for the DatabaseAdmin service.
 * Hand-written for the same reason as InstanceAdminGrpcClient.
 */
final class DatabaseAdminGrpcClient extends BaseStub
{
    public function CreateDatabase(
        CreateDatabaseRequest $argument,
        array $metadata = [],
        array $options = [],
    ): UnaryCall {
        return $this->_simpleRequest(
            '/google.spanner.admin.database.v1.DatabaseAdmin/CreateDatabase',
            $argument,
            [Operation::class, 'decode'],
            $metadata,
            $options,
        );
    }

    public function DropDatabase(
        DropDatabaseRequest $argument,
        array $metadata = [],
        array $options = [],
    ): UnaryCall {
        return $this->_simpleRequest(
            '/google.spanner.admin.database.v1.DatabaseAdmin/DropDatabase',
            $argument,
            [GPBEmpty::class, 'decode'],
            $metadata,
            $options,
        );
    }

    public function ListDatabases(
        ListDatabasesRequest $argument,
        array $metadata = [],
        array $options = [],
    ): UnaryCall {
        return $this->_simpleRequest(
            '/google.spanner.admin.database.v1.DatabaseAdmin/ListDatabases',
            $argument,
            [ListDatabasesResponse::class, 'decode'],
            $metadata,
            $options,
        );
    }
}
