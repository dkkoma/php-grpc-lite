--TEST--
grpc internal objects reject clone, double construction, and uninitialized use
--SKIPIF--
<?php
if (!extension_loaded('grpc')) {
    die('skip grpc extension not loaded');
}
?>
--FILE--
<?php
declare(strict_types=1);

require __DIR__ . '/helpers.inc';

Grpc\ChannelCredentials::invalidateDefaultRootsPem();
grpc_lite_phpt_assert_true(!Grpc\ChannelCredentials::isDefaultRootsPemSet(), 'default roots initially unset');
Grpc\ChannelCredentials::setDefaultRootsPem("-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----\n");
grpc_lite_phpt_assert_true(Grpc\ChannelCredentials::isDefaultRootsPemSet(), 'default roots set');
$defaultCredentials = Grpc\ChannelCredentials::createDefault();
$sslDefaultCredentials = Grpc\ChannelCredentials::createSsl();
Grpc\ChannelCredentials::invalidateDefaultRootsPem();
grpc_lite_phpt_assert_true(!Grpc\ChannelCredentials::isDefaultRootsPemSet(), 'default roots invalidated');
unset($defaultCredentials, $sslDefaultCredentials);
grpc_lite_phpt_expect_throw(static fn () => Grpc\CallCredentials::createFromPlugin('not-a-callable'), 'callable');

$channel = new Grpc\Channel('test-server:50051', [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
]);
$channelWithoutPort = new Grpc\Channel('test-server', [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
    'grpc.default_authority' => 'authority.example',
    'grpc.ssl_target_name_override' => 'override.example',
    'grpc.primary_user_agent' => 'php-grpc-lite-test',
    'grpc.max_receive_message_length' => -1,
]);
grpc_lite_phpt_assert_same('test-server', $channelWithoutPort->getTarget(), 'target without explicit port');
$call = new Grpc\Call($channel, '/helloworld.Greeter/SayHello', Grpc\Timeval::infFuture());

foreach ([
    Grpc\ChannelCredentials::createInsecure(),
    Grpc\CallCredentials::createFromPlugin(static fn (): array => []),
    Grpc\Timeval::infFuture(),
    $channel,
    $call,
] as $object) {
    grpc_lite_phpt_expect_throw(static fn () => clone $object, 'uncloneable', Error::class);
}

grpc_lite_phpt_expect_throw(static fn () => $channel->__construct('test-server:50051', [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
]));
grpc_lite_phpt_expect_throw(static fn () => (new ReflectionClass(Grpc\Channel::class))->newInstanceWithoutConstructor()->getTarget());
grpc_lite_phpt_expect_throw(static fn () => new Grpc\Channel("test-server\0:50051", [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
]));
grpc_lite_phpt_expect_throw(static fn () => new Grpc\Channel('test-server:not-a-port', [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
]));
grpc_lite_phpt_expect_throw(static fn () => new Grpc\Channel('test-server:50051', []), 'ChannelCredentials');
grpc_lite_phpt_expect_throw(static fn () => new Grpc\Channel('test-server:50051', [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
    'grpc.max_receive_message_length' => -2,
]), 'grpc.max_receive_message_length');
grpc_lite_phpt_expect_throw(static fn () => new Grpc\Channel('test-server:50051', [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
    'grpc.max_metadata_size' => -1,
]), 'grpc.max_metadata_size');
grpc_lite_phpt_expect_throw(static fn () => new Grpc\Channel('test-server:50051', [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
    'grpc.absolute_max_metadata_size' => -1,
]), 'grpc.absolute_max_metadata_size');
grpc_lite_phpt_expect_throw(static fn () => new Grpc\Channel('test-server:50051', [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
    'grpc.max_metadata_size' => 1024,
    'grpc.absolute_max_metadata_size' => 512,
]));
grpc_lite_phpt_expect_throw(static fn () => new Grpc\Channel('test-server:50051', [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
    'grpc.default_authority' => 'bad/authority',
]), 'authority');
grpc_lite_phpt_expect_throw(static fn () => new Grpc\Channel('test-server:50051', [
    'credentials' => Grpc\ChannelCredentials::createSsl(),
    'grpc.ssl_target_name_override' => 'bad/name',
]), 'TLS verify name');
grpc_lite_phpt_expect_throw(static fn () => new Grpc\Call($channel, '/bad method/Name', Grpc\Timeval::infFuture()));
grpc_lite_phpt_expect_throw(static fn () => (new ReflectionClass(Grpc\Call::class))->newInstanceWithoutConstructor()->getPeer());
grpc_lite_phpt_expect_throw(static fn () => (new ReflectionClass(Grpc\Call::class))->newInstanceWithoutConstructor()->startBatch([]));

$failed = (new ReflectionClass(Grpc\Channel::class))->newInstanceWithoutConstructor();
try {
    $failed->__construct("test-server\0:50051", [
        'credentials' => Grpc\ChannelCredentials::createInsecure(),
    ]);
} catch (Throwable) {
}
$failed->__construct('test-server:50051', [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
]);
grpc_lite_phpt_assert_same('test-server:50051', $failed->getTarget(), 'construct after failed construct');

$channel->close();
grpc_lite_phpt_assert_same('test-server:50051', $channel->getTarget(), 'channel target after close');
grpc_lite_phpt_assert_same(2, $channel->getConnectivityState(), 'connectivity state');
grpc_lite_phpt_assert_same(2, $channel->getConnectivityState(true), 'connectivity state try connect');
grpc_lite_phpt_assert_same(false, $channel->watchConnectivityState(2, Grpc\Timeval::zero()), 'watch connectivity state');
grpc_lite_phpt_expect_throw(static fn () => (new ReflectionClass(Grpc\Channel::class))->newInstanceWithoutConstructor()->getConnectivityState());
grpc_lite_phpt_expect_throw(static fn () => (new ReflectionClass(Grpc\Channel::class))->newInstanceWithoutConstructor()->watchConnectivityState(2, Grpc\Timeval::zero()));
grpc_lite_phpt_expect_throw(static fn () => (new ReflectionClass(Grpc\Channel::class))->newInstanceWithoutConstructor()->close());
$call->setCredentials(Grpc\CallCredentials::createFromPlugin(static fn (): array => []));
grpc_lite_phpt_assert_same('test-server:50051', $call->getPeer(), 'call peer');
grpc_lite_phpt_expect_throw(static fn () => $call->startBatch([
    Grpc\OP_SEND_MESSAGE => 'not-array',
]), 'OP_SEND_MESSAGE');
grpc_lite_phpt_expect_throw(static fn () => $call->startBatch([
    Grpc\OP_SEND_MESSAGE => ['message' => 123],
]), 'OP_SEND_MESSAGE');

$noMessageCall = new Grpc\Call($channel, '/helloworld.Greeter/SayHello', Grpc\Timeval::infFuture());
grpc_lite_phpt_expect_throw(static fn () => $noMessageCall->startBatch([
    Grpc\OP_RECV_STATUS_ON_CLIENT => true,
]), 'request message');

$insecureCredentialsCallbackCalled = false;
$insecureCredentialsCall = new Grpc\Call($channel, '/helloworld.Greeter/SayHello', Grpc\Timeval::infFuture());
$insecureCredentialsCall->setCredentials(Grpc\CallCredentials::createFromPlugin(static function () use (&$insecureCredentialsCallbackCalled): array {
    $insecureCredentialsCallbackCalled = true;
    return ['authorization' => 'Bearer token'];
}));
$insecureCredentialsEvent = $insecureCredentialsCall->startBatch([
    Grpc\OP_SEND_MESSAGE => ['message' => ''],
    Grpc\OP_RECV_STATUS_ON_CLIENT => true,
]);
grpc_lite_phpt_assert_true(!$insecureCredentialsCallbackCalled, 'insecure call credentials callback must not be called');
grpc_lite_phpt_assert_same(Grpc\STATUS_UNAUTHENTICATED, $insecureCredentialsEvent->status->code, 'insecure call credentials status');

$call->cancel();
$cancelledEvent = $call->startBatch([
    Grpc\OP_RECV_INITIAL_METADATA => true,
    Grpc\OP_RECV_MESSAGE => true,
    Grpc\OP_RECV_STATUS_ON_CLIENT => true,
]);
grpc_lite_phpt_assert_same(Grpc\STATUS_CANCELLED, $cancelledEvent->status->code, 'cancelled call status');
grpc_lite_phpt_expect_throw(static fn () => (new ReflectionClass(Grpc\Call::class))->newInstanceWithoutConstructor()->cancel());
grpc_lite_phpt_expect_throw(static fn () => (new ReflectionClass(Grpc\Call::class))->newInstanceWithoutConstructor()->setCredentials(Grpc\CallCredentials::createFromPlugin(static fn (): array => [])));

echo "OK\n";
?>
--EXPECT--
OK
