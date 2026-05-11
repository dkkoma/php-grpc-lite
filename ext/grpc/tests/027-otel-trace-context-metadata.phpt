--TEST--
OpenTelemetry trace context helper injects active context into gRPC metadata
--FILE--
<?php
declare(strict_types=1);

namespace OpenTelemetry\API\Trace\Propagation {
    final class TraceContextPropagator
    {
        public static function getInstance(): self
        {
            return new self();
        }

        /**
         * @param array<string, string> $carrier
         */
        public function inject(array &$carrier): void
        {
            $carrier['traceparent'] = '00-0123456789abcdef0123456789abcdef-0123456789abcdef-01';
            $carrier['tracestate'] = 'vendor=value';
        }
    }
}

namespace Grpc {
    class Interceptor
    {
    }
}

namespace {
    require __DIR__ . '/helpers.inc';
    require grpc_lite_phpt_repo_root() . '/src/OpenTelemetry/TraceContextMetadata.php';
    require grpc_lite_phpt_repo_root() . '/src/OpenTelemetry/TraceContextInterceptor.php';

    $metadata = \GrpcLite\OpenTelemetry\TraceContextMetadata::inject([
        'x-existing' => ['1'],
    ]);

    grpc_lite_phpt_assert_same(['1'], $metadata['x-existing'], 'existing metadata');
    grpc_lite_phpt_assert_same(
        ['00-0123456789abcdef0123456789abcdef-0123456789abcdef-01'],
        $metadata['traceparent'],
        'traceparent metadata',
    );
    grpc_lite_phpt_assert_same(['vendor=value'], $metadata['tracestate'], 'tracestate metadata');

    $callback = \GrpcLite\OpenTelemetry\TraceContextMetadata::updateMetadataCallback();
    $callbackMetadata = $callback([], 'https://example.test/service');
    grpc_lite_phpt_assert_same($metadata['traceparent'], $callbackMetadata['traceparent'], 'update_metadata callback traceparent');

    $interceptor = new \GrpcLite\OpenTelemetry\TraceContextInterceptor();
    $called = false;
    $interceptor->interceptUnaryUnary(
        '/example.Service/Method',
        new \stdClass(),
        static fn(): mixed => null,
        static function ($method, $argument, $deserialize, array $metadata, array $options) use (&$called): string {
            $called = true;
            grpc_lite_phpt_assert_same('/example.Service/Method', $method, 'interceptor method');
            grpc_lite_phpt_assert_same(['00-0123456789abcdef0123456789abcdef-0123456789abcdef-01'], $metadata['traceparent'], 'interceptor traceparent');
            grpc_lite_phpt_assert_same(['flag' => true], $options, 'interceptor options');
            return 'ok';
        },
        [],
        ['flag' => true],
    );
    grpc_lite_phpt_assert_true($called, 'interceptor continuation called');

    echo "OK\n";
}
?>
--EXPECT--
OK
