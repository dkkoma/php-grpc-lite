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
Grpc\ChannelCredentials::invalidateDefaultRootsPem();
grpc_lite_phpt_assert_true(!Grpc\ChannelCredentials::isDefaultRootsPemSet(), 'default roots invalidated');

$channel = new Grpc\Channel('test-server:50051', [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
]);
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
grpc_lite_phpt_expect_throw(static fn () => new Grpc\Channel('test-server:50051', [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
    'grpc.max_metadata_size' => 1024,
    'grpc.absolute_max_metadata_size' => 512,
]));
grpc_lite_phpt_expect_throw(static fn () => new Grpc\Call($channel, '/bad method/Name', Grpc\Timeval::infFuture()));
grpc_lite_phpt_expect_throw(static fn () => (new ReflectionClass(Grpc\Call::class))->newInstanceWithoutConstructor()->getPeer());

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

echo "OK\n";
?>
--EXPECT--
OK
