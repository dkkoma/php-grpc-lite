--TEST--
grpc telemetry PHP adapters attach records to active tracing spans
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
?>
--FILE--
<?php
declare(strict_types=1);

namespace OpenTelemetry\API\Trace {
    final class Span
    {
        public static ?FakeSpan $current = null;

        public static function getCurrent(): ?FakeSpan
        {
            return self::$current;
        }
    }

    final class FakeSpan
    {
        /** @var array<string, mixed> */
        public array $attributes = [];
        /** @var list<array{name: string, attributes: array<string, mixed>}> */
        public array $events = [];

        public function setAttribute(string $key, mixed $value): void
        {
            $this->attributes[$key] = $value;
        }

        /**
         * @param array<string, mixed> $attributes
         */
        public function addEvent(string $name, array $attributes = []): void
        {
            $this->events[] = ['name' => $name, 'attributes' => $attributes];
        }
    }
}

namespace DDTrace {
    final class FakeSpan
    {
        /** @var array<string, string> */
        public array $meta = [];
        /** @var array<string, float> */
        public array $metrics = [];
    }

    $activeSpan = null;

    function active_span(): ?FakeSpan
    {
        global $activeSpan;
        return $activeSpan;
    }
}

namespace {
    require __DIR__ . '/helpers.inc';
    grpc_lite_phpt_require_autoload();
    require_once __DIR__ . '/../../../src/Telemetry/Telemetry.php';
    require_once __DIR__ . '/../../../src/Telemetry/OpenTelemetryHandler.php';
    require_once __DIR__ . '/../../../src/Telemetry/DdTraceHandler.php';

    use DDTrace\FakeSpan as DdFakeSpan;
    use GrpcLite\Telemetry\DdTraceHandler;
    use GrpcLite\Telemetry\OpenTelemetryHandler;
    use GrpcLite\Telemetry\Telemetry;
    use OpenTelemetry\API\Trace\FakeSpan as OtelFakeSpan;
    use OpenTelemetry\API\Trace\Span;

    $record = [
        'rpc_service' => 'helloworld.Greeter',
        'rpc_method' => 'BenchUnary',
        'grpc_status_code' => 0,
        'http_status_code' => 200,
        'backend' => 'http2',
        'duration_us' => 123,
        'timings' => ['recv_loop_us' => 45],
        'sizes' => ['request_bytes' => 6],
        'http2' => ['stream_id' => 1, 'stream_reset_seen' => false],
        'connection' => ['persistent_reused' => true],
    ];

    Telemetry::setHandler(static function (array $record): void {});
    grpc_lite_phpt_assert_same('1', ini_get('grpc_lite.telemetry_enabled'), 'telemetry helper enables ini');
    Telemetry::setHandler(null);
    grpc_lite_phpt_assert_same('0', ini_get('grpc_lite.telemetry_enabled'), 'telemetry helper disables ini');

    Span::$current = new OtelFakeSpan();
    (new OpenTelemetryHandler())($record);
    grpc_lite_phpt_assert_same('helloworld.Greeter', Span::$current->attributes['rpc.service'], 'otel rpc service attribute');
    grpc_lite_phpt_assert_same(123, Span::$current->attributes['grpc_lite.duration_us'], 'otel duration attribute');
    grpc_lite_phpt_assert_same('grpc.client', Span::$current->events[0]['name'], 'otel grpc client event');

    $GLOBALS['activeSpan'] = new DdFakeSpan();
    (new DdTraceHandler())($record);
    grpc_lite_phpt_assert_same('helloworld.Greeter', $GLOBALS['activeSpan']->meta['rpc.service'], 'ddtrace rpc service meta');
    grpc_lite_phpt_assert_same(123.0, $GLOBALS['activeSpan']->metrics['grpc_lite.duration_us'], 'ddtrace duration metric');
    grpc_lite_phpt_assert_same(1.0, $GLOBALS['activeSpan']->metrics['grpc_lite.connection_persistent_reused'], 'ddtrace bool metric');

    echo "OK\n";
}
?>
--EXPECT--
OK
