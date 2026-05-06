# Native extension install guide

`php-grpc-lite` のPHP userland実装はComposerで導入し、HTTP/2 transport extensionはこのrepositoryをcloneしてsource buildする。

このextensionはPHP module名として `grpc` を使い、`grpc.so` を生成する。これはdrop-in検証のために `extension_loaded('grpc')` を満たす設計であり、公式 `grpc/grpc` の `ext-grpc` と同時にloadしてはいけない。

## Install model

- PHP classes: Composer package `php-grpc-lite/php-grpc-lite` が `Grpc\*` API surfaceをautoloadする。
- Native extension: このrepositoryの `ext/grpc/` を `phpize` でbuildする。
- Runtime transport: HTTP/2 transportのみ。release readiness is still gated by C extension memory/lifecycle QA.
- Composer metadata: package は `ext-grpc` を `provide` するが、Composerはsource-built grpc extensionをbuild/loadしない。source buildと `extension=grpc.so` の有効化を完了してから、drop-in replacementとして扱う。
- PIE packaging: `grpc-php-rs` はroot packageを `type: php-ext` にして `pie install bsn4/grpc` を提供している。このrepositoryはPHP userland libraryとextension sourceを同居させるため、root packageは `type: library` のままにし、PIE対応時は別のextension packageとして切り出す。
- Rollback:
  - 公式 `ext-grpc` へ戻す場合は、このextensionの `extension=grpc.so` を無効化し、公式側の `grpc.so` を有効化する。

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

`Grpc\BaseStub`、`Grpc\UnaryCall`、`Grpc\ServerStreamingCall` などの高レベル wrapper はComposer autoloadが提供する。`Grpc\Channel`、credentials、`Grpc\Timeval`、`Grpc\STATUS_*` などの低レベルsurfaceは、公式 `ext-grpc` と同じくsource-built grpc extension側で提供する方針へ移行する。

## Build source-built grpc extension

このrepositoryをcloneし、`ext/grpc/` をbuildする。

```bash
git clone <php-grpc-lite repository URL> php-grpc-lite
cd php-grpc-lite/ext/grpc
phpize
./configure --enable-grpc
make -j"$(nproc)"
sudo make install
```

開発container内で検証する場合:

```bash
docker compose run --rm dev sh -lc 'cd ext/grpc && phpize && ./configure --enable-grpc && make -j$(nproc)'
docker compose run --rm dev php -d extension=/workspace/ext/grpc/modules/grpc.so -r 'var_dump(extension_loaded("grpc"), function_exists("grpc_lite_unary"));'
```

## Enable extension

`make install` が出力したinstall先に合わせて、PHPの設定へ `extension=grpc.so` を追加する。

例:

```bash
echo 'extension=grpc.so' | sudo tee /etc/php/conf.d/20-php-grpc-lite.ini
php -m | grep '^grpc$'
php -r 'var_dump(extension_loaded("grpc"), function_exists("grpc_lite_unary"));'
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

transport選択optionはない。source-built grpc extension未ロード、未対応機能、transport errorは別経路へfallbackせず、選択されたRPCの失敗として返す。

## Compatibility scope

現時点のdrop-in replacement対象は unary と server streaming。client streaming と bidirectional streaming は API surface だけを予約しており、呼び出すと明示的な未実装例外を投げる。

Pub/Sub StreamingPull など bidi streaming を使うclientは、現時点では公式 `ext-grpc` を使う。

## Verification

```bash
php -r 'require "vendor/autoload.php"; var_dump(extension_loaded("grpc"), function_exists("grpc_lite_unary"), class_exists(Grpc\\Channel::class));'
```

期待値:

- `extension_loaded("grpc") === true`
- `function_exists("grpc_lite_unary") === true`
- `class_exists(Grpc\Channel::class) === true`

`extension_loaded("grpc")` だけでは公式 `ext-grpc` と区別できないため、`grpc_lite_unary()` の存在も確認する。

通常buildでは production bridge のみを公開する。`ext/grpc/bench.php` 用の診断entrypointが必要な場合だけ、開発用途として `./configure --enable-grpc --enable-grpc-bench` でbuildする。

このrepositoryのDocker環境では、HTTP/2 stream lifecycle smokeも確認できる。

```bash
ITERATIONS=10 BENCH_TAG=install-smoke ./bench/phase2/check-native-lifecycle-stress.sh
```

release hardeningに近い確認をまとめて実行する場合:

```bash
BENCH_TAG=release-hardening ./bench/phase2/check-native-release-hardening.sh
```

このrunnerは以下を実行する。

- C extension static analysis (`cppcheck`)
- lifecycle stress smoke
- Valgrind lifecycle smoke
- long lifecycle stress
- PHP-FPM request boundaryでのpersistent channel reuse確認

## Known limitation

large server streaming bulk transferは事前ベンチを推奨する。目安は `>=64KiB/message` かつ `>=8MiB/stream`、またはlarge payload `>=50 messages`。

この範囲でp99やthroughputがSLOに入るかは、実ワークロードに近いshapeで事前に測定する。
