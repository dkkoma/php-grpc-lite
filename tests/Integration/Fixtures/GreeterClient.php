<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration\Fixtures;

use Grpc\BaseStub;
use Grpc\UnaryCall;
use Helloworld\HelloReply;
use Helloworld\HelloRequest;

/**
 * Hand-written analogue of what `protoc-gen-php-grpc` would emit for the
 * Greeter service. Used only by the integration tests to exercise the
 * BaseStub surface end-to-end against the local test server.
 */
class GreeterClient extends BaseStub
{
    public function SayHello(
        HelloRequest $argument,
        array $metadata = [],
        array $options = [],
    ): UnaryCall {
        return $this->_simpleRequest(
            '/helloworld.Greeter/SayHello',
            $argument,
            [HelloReply::class, 'decode'],
            $metadata,
            $options,
        );
    }
}
