<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration\Fixtures;

use Grpc\BaseStub;
use Grpc\ServerStreamingCall;
use Grpc\UnaryCall;
use Helloworld\BenchReply;
use Helloworld\BenchRequest;
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

    public function SayManyHellos(
        HelloRequest $argument,
        array $metadata = [],
        array $options = [],
    ): ServerStreamingCall {
        return $this->_serverStreamRequest(
            '/helloworld.Greeter/SayManyHellos',
            $argument,
            [HelloReply::class, 'decode'],
            $metadata,
            $options,
        );
    }

    public function BenchUnary(
        BenchRequest $argument,
        array $metadata = [],
        array $options = [],
    ): UnaryCall {
        return $this->_simpleRequest(
            '/helloworld.Greeter/BenchUnary',
            $argument,
            [BenchReply::class, 'decode'],
            $metadata,
            $options,
        );
    }

    public function BenchServerStream(
        BenchRequest $argument,
        array $metadata = [],
        array $options = [],
    ): ServerStreamingCall {
        return $this->_serverStreamRequest(
            '/helloworld.Greeter/BenchServerStream',
            $argument,
            [BenchReply::class, 'decode'],
            $metadata,
            $options,
        );
    }
}
