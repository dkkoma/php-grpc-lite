<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration\Fixtures\Spanner;

use Google\Cloud\Spanner\Admin\Instance\V1\CreateInstanceRequest;
use Google\Cloud\Spanner\Admin\Instance\V1\DeleteInstanceRequest;
use Google\Cloud\Spanner\Admin\Instance\V1\ListInstancesRequest;
use Google\Cloud\Spanner\Admin\Instance\V1\ListInstancesResponse;
use Google\LongRunning\Operation;
use Google\Protobuf\GPBEmpty;
use Grpc\BaseStub;
use Grpc\UnaryCall;

/**
 * Minimal hand-written analogue of what `protoc-gen-php-grpc` would generate
 * for the InstanceAdmin service. Exists because google/cloud-spanner v2.x
 * ships only the GAPIC (gax-driven) client, which currently can't run on
 * Phase 0 because gax checks `extension_loaded('grpc')`.
 *
 * Add new RPCs here as the staged Spanner verification plan progresses
 * (CreateInstance, etc.).
 */
final class InstanceAdminGrpcClient extends BaseStub
{
    public function ListInstances(
        ListInstancesRequest $argument,
        array $metadata = [],
        array $options = [],
    ): UnaryCall {
        return $this->_simpleRequest(
            '/google.spanner.admin.instance.v1.InstanceAdmin/ListInstances',
            $argument,
            [ListInstancesResponse::class, 'decode'],
            $metadata,
            $options,
        );
    }

    public function CreateInstance(
        CreateInstanceRequest $argument,
        array $metadata = [],
        array $options = [],
    ): UnaryCall {
        return $this->_simpleRequest(
            '/google.spanner.admin.instance.v1.InstanceAdmin/CreateInstance',
            $argument,
            [Operation::class, 'decode'],
            $metadata,
            $options,
        );
    }

    public function DeleteInstance(
        DeleteInstanceRequest $argument,
        array $metadata = [],
        array $options = [],
    ): UnaryCall {
        return $this->_simpleRequest(
            '/google.spanner.admin.instance.v1.InstanceAdmin/DeleteInstance',
            $argument,
            [GPBEmpty::class, 'decode'],
            $metadata,
            $options,
        );
    }
}
