# Native extension install guide

`php-grpc-lite` のPHP userland実装はComposerで導入し、native transport extensionはこのrepositoryをcloneしてsource buildする。

このextensionはPHP module名として `grpc` を使い、`grpc.so` を生成する。これはdrop-in検証のために `extension_loaded('grpc')` を満たす設計であり、公式 `grpc/grpc` の `ext-grpc` と同時にloadしてはいけない。

## Install model

- PHP classes: Composer package `php-grpc-lite/php-grpc-lite` が `Grpc\*` API surfaceをautoloadする。
- Native extension: このrepositoryの `ext/grpc/` を `phpize` でbuildする。
- Runtime default: `native` transport。
- Stable route: `php_grpc_lite.transport=curl` を明示した場合だけlibcurl経路を使う。
- Rollback: `extension=grpc.so` を外して公式 `ext-grpc` へ戻すか、アプリ側で `php_grpc_lite.transport=curl` を明示する。

## Requirements

- PHP 8.4+
- PHP development headers / `phpize`
- C compiler and make
- `pkg-config`
- `libnghttp2`
- OpenSSL development headers

Debian系の例:

```bash
sudo apt-get install -y php-dev build-essential pkg-config libnghttp2-dev libssl-dev
```

## PHP userland code

アプリケーションにはComposerでPHP codeを入れる。

```bash
composer require php-grpc-lite/php-grpc-lite
```

`Grpc\Channel`、`Grpc\BaseStub`、`Grpc\ChannelCredentials` などのclassはComposer autoloadが提供する。native extensionはこれらのclassをCで登録しない。

## Build native extension

このrepositoryをcloneし、`ext/grpc/` をbuildする。

```bash
git clone https://github.com/your-vendor/php-grpc-lite.git
cd php-grpc-lite/ext/grpc
phpize
./configure --enable-grpc
make -j"$(nproc)"
sudo make install
```

開発container内で検証する場合:

```bash
docker compose run --rm dev sh -lc 'cd ext/grpc && phpize && ./configure --enable-grpc && make -j$(nproc)'
docker compose run --rm dev php -d extension=/workspace/ext/grpc/modules/grpc.so -r 'var_dump(extension_loaded("grpc"));'
```

## Enable extension

`make install` が出力したinstall先に合わせて、PHPの設定へ `extension=grpc.so` を追加する。

例:

```bash
echo 'extension=grpc.so' | sudo tee /etc/php/conf.d/20-php-grpc-lite.ini
php -m | grep '^grpc$'
php -r 'var_dump(extension_loaded("grpc"));'
```

公式 `ext-grpc` が既に有効な環境では、先に公式側の `extension=grpc.so` 設定を外す。同名moduleなので同時loadはできない。

## Application usage

通常のgenerated stub / gax clientはComposer autoload経由で使う。

```php
require __DIR__ . '/vendor/autoload.php';

$client = new SomeGrpcClient('example.com:443', [
    'credentials' => Grpc\ChannelCredentials::createSsl(),
]);
```

transportを明示する場合:

```php
$client = new SomeGrpcClient($target, [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
    'php_grpc_lite.transport' => 'native',
]);

$stableClient = new SomeGrpcClient($target, [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
    'php_grpc_lite.transport' => 'curl',
]);
```

`curl` は自動fallbackではなく、workloadや運用安定性に応じてユーザーが明示選択するstable route。

## Verification

```bash
php -r 'require "vendor/autoload.php"; var_dump(extension_loaded("grpc"), class_exists(Grpc\\Channel::class));'
```

期待値:

- `extension_loaded("grpc") === true`
- `class_exists(Grpc\Channel::class) === true`

## Known limitation

large server streaming bulk transferは事前ベンチを推奨する。目安は `>=64KiB/message` かつ `>=8MiB/stream`、またはlarge payload `>=50 messages`。

この範囲でp99やthroughputがSLOに入る場合は、`native` と `curl` の両方を実ワークロードに近いshapeで比較する。
