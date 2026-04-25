<?php
declare(strict_types=1);

namespace PhpGrpcLite\Tests\Integration;

use Grpc\ChannelCredentials;
use Grpc\Channel;
use Grpc\Interceptor;
use Grpc\UnaryCall;
use Helloworld\HelloRequest;
use PhpGrpcLite\Tests\Integration\Fixtures\GreeterClient;
use PHPUnit\Framework\Attributes\Group;
use PHPUnit\Framework\TestCase;

#[Group('integration')]
final class InterceptorTest extends TestCase
{
    private const TARGET = 'test-server:50051';

    public function testInterceptorChainModifiesMetadataAndExecutionOrder(): void
    {
        $observed = [];

        $outer = new class($observed) extends Interceptor {
            public function __construct(private array &$observed) {}

            public function interceptUnaryUnary(
                string $method,
                object $argument,
                $deserialize,
                callable $continuation,
                array $metadata = [],
                array $options = [],
            ): UnaryCall {
                $this->observed[] = 'outer:before';
                $metadata['x-outer'] = ['outer-value'];
                $call = $continuation($method, $argument, $deserialize, $metadata, $options);
                $this->observed[] = 'outer:after';
                return $call;
            }
        };

        $inner = new class($observed) extends Interceptor {
            public function __construct(private array &$observed) {}

            public function interceptUnaryUnary(
                string $method,
                object $argument,
                $deserialize,
                callable $continuation,
                array $metadata = [],
                array $options = [],
            ): UnaryCall {
                $this->observed[] = 'inner:before';
                $metadata['x-inner'] = ['inner-value'];
                $call = $continuation($method, $argument, $deserialize, $metadata, $options);
                $this->observed[] = 'inner:after';
                return $call;
            }
        };

        $opts = ['credentials' => ChannelCredentials::createInsecure()];
        $channel = Interceptor::intercept(new Channel(self::TARGET, $opts), [$outer, $inner]);

        $client = new GreeterClient(self::TARGET, $opts, $channel);
        $request = new HelloRequest();
        $request->setName('Intercepted');

        $call = $client->SayHello($request);
        [$response, $status] = $call->wait();

        self::assertSame(\Grpc\STATUS_OK, $status->code);
        self::assertSame('Hello, Intercepted', $response->getMessage());

        // Outer wraps inner: outer:before → inner:before → (call) → inner:after → outer:after
        self::assertSame(['outer:before', 'inner:before', 'inner:after', 'outer:after'], $observed);
    }
}
