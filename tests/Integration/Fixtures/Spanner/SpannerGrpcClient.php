<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration\Fixtures\Spanner;

use Google\Cloud\Spanner\V1\CreateSessionRequest;
use Google\Cloud\Spanner\V1\BeginTransactionRequest;
use Google\Cloud\Spanner\V1\CommitRequest;
use Google\Cloud\Spanner\V1\CommitResponse;
use Google\Cloud\Spanner\V1\DeleteSessionRequest;
use Google\Cloud\Spanner\V1\ExecuteSqlRequest;
use Google\Cloud\Spanner\V1\PartialResultSet;
use Google\Cloud\Spanner\V1\ResultSet;
use Google\Cloud\Spanner\V1\Session;
use Google\Cloud\Spanner\V1\Transaction;
use Google\Protobuf\GPBEmpty;
use Grpc\BaseStub;
use Grpc\ServerStreamingCall;
use Grpc\UnaryCall;

/**
 * protoc-gen-php-grpc-equivalent for the Spanner data plane service.
 */
final class SpannerGrpcClient extends BaseStub
{
    public function CreateSession(
        CreateSessionRequest $argument,
        array $metadata = [],
        array $options = [],
    ): UnaryCall {
        return $this->_simpleRequest(
            '/google.spanner.v1.Spanner/CreateSession',
            $argument,
            [Session::class, 'decode'],
            $metadata,
            $options,
        );
    }

    public function DeleteSession(
        DeleteSessionRequest $argument,
        array $metadata = [],
        array $options = [],
    ): UnaryCall {
        return $this->_simpleRequest(
            '/google.spanner.v1.Spanner/DeleteSession',
            $argument,
            [GPBEmpty::class, 'decode'],
            $metadata,
            $options,
        );
    }

    public function BeginTransaction(
        BeginTransactionRequest $argument,
        array $metadata = [],
        array $options = [],
    ): UnaryCall {
        return $this->_simpleRequest(
            '/google.spanner.v1.Spanner/BeginTransaction',
            $argument,
            [Transaction::class, 'decode'],
            $metadata,
            $options,
        );
    }

    public function Commit(
        CommitRequest $argument,
        array $metadata = [],
        array $options = [],
    ): UnaryCall {
        return $this->_simpleRequest(
            '/google.spanner.v1.Spanner/Commit',
            $argument,
            [CommitResponse::class, 'decode'],
            $metadata,
            $options,
        );
    }

    public function ExecuteSql(
        ExecuteSqlRequest $argument,
        array $metadata = [],
        array $options = [],
    ): UnaryCall {
        return $this->_simpleRequest(
            '/google.spanner.v1.Spanner/ExecuteSql',
            $argument,
            [ResultSet::class, 'decode'],
            $metadata,
            $options,
        );
    }

    public function ExecuteStreamingSql(
        ExecuteSqlRequest $argument,
        array $metadata = [],
        array $options = [],
    ): ServerStreamingCall {
        return $this->_serverStreamRequest(
            '/google.spanner.v1.Spanner/ExecuteStreamingSql',
            $argument,
            [PartialResultSet::class, 'decode'],
            $metadata,
            $options,
        );
    }
}
