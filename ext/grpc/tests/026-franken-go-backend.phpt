--TEST--
grpc_lite franken-go backend delegates unary and server streaming calls
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
?>
--FILE--
<?php
declare(strict_types=1);

namespace FrankenGrpc {
    final class Channel
    {
        public bool $closed = false;

        public function __construct(
            public string $target,
            public array $options = [],
        ) {
        }

        public function close(): void
        {
            $this->closed = true;
        }
    }

    final class UnaryCall
    {
        public static int $starts = 0;

        public function __construct(
            public Channel $channel,
            public string $method,
        ) {
        }

        public function start(string $payload, array $metadata = [], ?float $timeoutSeconds = null): UnaryResult
        {
            self::$starts++;
            if ($this->channel->target !== 'franken.test:443') {
                throw new \RuntimeException('target was not delegated');
            }
            if ($this->method !== '/pkg.Service/Unary') {
                throw new \RuntimeException('method was not delegated');
            }
            if ($payload !== 'request-bytes') {
                throw new \RuntimeException('payload was not delegated');
            }
            if (($metadata['x-test'][0] ?? null) !== 'yes') {
                throw new \RuntimeException('metadata was not delegated');
            }
            if (($metadata['user-agent'][0] ?? null) !== 'php-grpc-lite/0.1.0') {
                throw new \RuntimeException('user-agent was not delegated');
            }
            if ($timeoutSeconds === null || $timeoutSeconds <= 0) {
                throw new \RuntimeException('timeout was not delegated');
            }

            return new UnaryResult(
                'response-bytes',
                new Status(\Grpc\STATUS_OK, '', ['grpc-status' => ['0']]),
                ['content-type' => ['application/grpc']],
                ['grpc-status' => ['0']],
            );
        }
    }

    final class ServerStreamingCall
    {
        public static int $starts = 0;

        private int $offset = 0;

        public function __construct(
            public Channel $channel,
            public string $method,
        ) {
        }

        public function start(string $payload, array $metadata = [], ?float $timeoutSeconds = null): void
        {
            self::$starts++;
            if ($this->method !== '/pkg.Service/Stream' || $payload !== 'stream-request') {
                throw new \RuntimeException('stream start was not delegated');
            }
        }

        public function read(): ?string
        {
            return match ($this->offset++) {
                0 => 'message-1',
                1 => 'message-2',
                default => null,
            };
        }

        public function getInitialMetadata(): array
        {
            return ['content-type' => ['application/grpc']];
        }

        public function getStatus(): Status
        {
            return new Status(\Grpc\STATUS_OK, '', ['grpc-status' => ['0']]);
        }

        public function getTrailingMetadata(): array
        {
            return ['grpc-status' => ['0']];
        }

        public function cancel(): void
        {
        }

        public function getPeer(): string
        {
            return $this->channel->target;
        }
    }

    final readonly class UnaryResult
    {
        public function __construct(
            public string $payload,
            public Status $status,
            public array $initialMetadata = [],
            public array $trailingMetadata = [],
        ) {
        }
    }

    final readonly class Status
    {
        public function __construct(
            public int $code,
            public string $details = '',
            public array $metadata = [],
        ) {
        }
    }
}

namespace {
    require __DIR__ . '/helpers.inc';

    $channel = new \Grpc\Channel('franken.test:443', [
        'credentials' => \Grpc\ChannelCredentials::createInsecure(),
        'grpc_lite.backend' => 'franken-go',
    ]);

    $unary = new \Grpc\Call($channel, '/pkg.Service/Unary', new \Grpc\Timeval(1000000));
    $unaryResult = $unary->startBatch([
        \Grpc\OP_SEND_INITIAL_METADATA => ['x-test' => ['yes']],
        \Grpc\OP_SEND_MESSAGE => ['message' => 'request-bytes'],
        \Grpc\OP_SEND_CLOSE_FROM_CLIENT => true,
        \Grpc\OP_RECV_INITIAL_METADATA => true,
        \Grpc\OP_RECV_MESSAGE => true,
        \Grpc\OP_RECV_STATUS_ON_CLIENT => true,
    ]);

    grpc_lite_phpt_assert_same('response-bytes', $unaryResult->message, 'franken unary message');
    grpc_lite_phpt_assert_same(\Grpc\STATUS_OK, $unaryResult->status->code, 'franken unary status');
    grpc_lite_phpt_assert_same('application/grpc', $unaryResult->metadata['content-type'][0] ?? null, 'franken unary initial metadata');

    $stream = new \Grpc\Call($channel, '/pkg.Service/Stream', new \Grpc\Timeval(1000000));
    $first = $stream->startBatch([
        \Grpc\OP_SEND_INITIAL_METADATA => [],
        \Grpc\OP_SEND_MESSAGE => ['message' => 'stream-request'],
        \Grpc\OP_SEND_CLOSE_FROM_CLIENT => true,
        \Grpc\OP_RECV_INITIAL_METADATA => true,
        \Grpc\OP_RECV_MESSAGE => true,
    ]);
    $second = $stream->startBatch([\Grpc\OP_RECV_MESSAGE => true]);
    $done = $stream->startBatch([\Grpc\OP_RECV_MESSAGE => true]);
    $status = $stream->startBatch([\Grpc\OP_RECV_STATUS_ON_CLIENT => true]);

    grpc_lite_phpt_assert_same('message-1', $first->message, 'franken stream first message');
    grpc_lite_phpt_assert_same('message-2', $second->message, 'franken stream second message');
    grpc_lite_phpt_assert_same(null, $done->message, 'franken stream done message');
    grpc_lite_phpt_assert_same(\Grpc\STATUS_OK, $status->status->code, 'franken stream status');

    $unaryCredentialsCallbackCalled = false;
    $unaryStartsBefore = \FrankenGrpc\UnaryCall::$starts;
    $insecureUnary = new \Grpc\Call($channel, '/pkg.Service/Unary', new \Grpc\Timeval(1000000));
    $insecureUnary->setCredentials(\Grpc\CallCredentials::createFromPlugin(static function () use (&$unaryCredentialsCallbackCalled): array {
        $unaryCredentialsCallbackCalled = true;
        return ['authorization' => 'Bearer token'];
    }));
    $insecureUnaryResult = $insecureUnary->startBatch([
        \Grpc\OP_SEND_MESSAGE => ['message' => 'request-bytes'],
        \Grpc\OP_RECV_STATUS_ON_CLIENT => true,
    ]);
    grpc_lite_phpt_assert_true(!$unaryCredentialsCallbackCalled, 'franken insecure unary credentials callback must not be called');
    grpc_lite_phpt_assert_same($unaryStartsBefore, \FrankenGrpc\UnaryCall::$starts, 'franken insecure unary must not delegate start');
    grpc_lite_phpt_assert_same(\Grpc\STATUS_UNAUTHENTICATED, $insecureUnaryResult->status->code, 'franken insecure unary credentials status');

    $streamCredentialsCallbackCalled = false;
    $streamStartsBefore = \FrankenGrpc\ServerStreamingCall::$starts;
    $insecureStream = new \Grpc\Call($channel, '/pkg.Service/Stream', new \Grpc\Timeval(1000000));
    $insecureStream->setCredentials(\Grpc\CallCredentials::createFromPlugin(static function () use (&$streamCredentialsCallbackCalled): array {
        $streamCredentialsCallbackCalled = true;
        return ['authorization' => 'Bearer token'];
    }));
    $insecureStreamFirst = $insecureStream->startBatch([
        \Grpc\OP_SEND_MESSAGE => ['message' => 'stream-request'],
        \Grpc\OP_RECV_MESSAGE => true,
    ]);
    $insecureStreamStatus = $insecureStream->startBatch([\Grpc\OP_RECV_STATUS_ON_CLIENT => true]);
    grpc_lite_phpt_assert_same(null, $insecureStreamFirst->message, 'franken insecure stream must not yield messages');
    grpc_lite_phpt_assert_true(!$streamCredentialsCallbackCalled, 'franken insecure stream credentials callback must not be called');
    grpc_lite_phpt_assert_same($streamStartsBefore, \FrankenGrpc\ServerStreamingCall::$starts, 'franken insecure stream must not delegate start');
    grpc_lite_phpt_assert_same(\Grpc\STATUS_UNAUTHENTICATED, $insecureStreamStatus->status->code, 'franken insecure stream credentials status');

    $channel->close();

    echo "OK\n";
}
?>
--EXPECT--
OK
