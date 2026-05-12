# Native extension install guide

`php-grpc-lite` はPIEでinstallするPHP extension packageとして扱う。アプリケーション側の高レベルPHP wrapperは公式 `grpc/grpc` Composer packageを使う。

このextensionはPHP module名として `grpc` を使い、`grpc.so` を生成する。これはdrop-in検証のために `extension_loaded('grpc')` を満たす設計であり、公式 `grpc/grpc` の `ext-grpc` と同時にloadしてはいけない。

## Install model

- PHP classes: 高レベル wrapper は公式 `grpc/grpc` Composer package が提供する。このpackage自体はComposer autoload用runtime codeを提供しない。
- Native extension: このrepository package自体を `type: php-ext` としてPIEでbuild/installする。sourceは `ext/grpc/`。
- Runtime transport: HTTP/2 transportのみ。release readiness is still gated by C extension memory/lifecycle QA.
- Composer metadata: root package は `type: php-ext`、`php-ext.extension-name: grpc`、`php-ext.build-path: ext/grpc` を持つ。Composer libraryとしてautoloadされるruntime codeは提供しない。
- PIE packaging: `pie install dkkoma/php-grpc-lite` を主install経路にする。PIEは `phpize` / `./configure` / `make` / `make install` を実行する。
- Rollback:
  - 公式 `ext-grpc` へ戻す場合は、このextensionの `extension=grpc` を無効化し、公式側の `grpc.so` を有効化する。

## Requirements

- PHP 8.4+
- PHP development headers / `phpize`
- C compiler and make
- `libnghttp2`
- OpenSSL development headers
- zip extractor for PIE/Composer dist archive extraction. Debian/Ubuntuでは `unzip` を推奨する。

Debian系の例:

```bash
sudo apt-get install -y php-dev build-essential libnghttp2-dev libssl-dev unzip
```

## Install with PIE

依存ライブラリを先に入れる。

```bash
sudo apt-get install -y php-dev build-essential libnghttp2-dev libssl-dev unzip
```

PIEでextensionをinstallする。

```bash
pie install dkkoma/php-grpc-lite --auto-install-build-tools --auto-install-system-dependencies
```

PIEはComposerの通常download経路を使う。安定版ではPackagist/GitHubのdist zipが優先されるため、zip展開手段が必要になる。Debian/Ubuntuでは `unzip` を推奨するが、Composer上はPHP `ext-zip` や `7z` でもよい。zip展開手段がない環境ではsource downloadへfallbackし、`git` が必要になる場合がある。

PIEが自動で有効化しない環境では、install先に合わせて `extension=grpc` をPHP設定へ追加する。

## PHP userland wrapper

アプリケーションにはComposerでPHP codeを入れる。

```bash
composer require grpc/grpc
```

`Grpc\BaseStub`、`Grpc\UnaryCall`、`Grpc\ServerStreamingCall` などの高レベル wrapper は公式 `grpc/grpc` Composer package が提供する。`Grpc\Channel`、`Grpc\Call`、credentials、`Grpc\Timeval`、`Grpc\STATUS_*` などの低レベルsurfaceは、このrepositoryのsource-built grpc extensionが提供する。

## Build source-built grpc extension without PIE

PIEを使わずに検証する場合は、このrepositoryをcloneし、`ext/grpc/` をbuildする。

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
docker compose run --rm dev php -d extension=/workspace/ext/grpc/modules/grpc.so -r 'var_dump(extension_loaded("grpc"), defined("Grpc\\VERSION") && constant("Grpc\\VERSION") === "0.1.0");'
```

公式 `php` image上でPIE install手順を検証する場合:

```bash
docker build -f Dockerfile.install-pie -t php-grpc-lite-install-pie .
docker run --rm php-grpc-lite-install-pie php -m | grep -x grpc
docker run --rm php-grpc-lite-install-pie php -r 'var_dump(extension_loaded("grpc"), defined("Grpc\\VERSION") && constant("Grpc\\VERSION") === "0.1.0");'
```

特定release packageをPackagist経由で検証する場合:

```bash
docker build -f Dockerfile.install-pie \
  --build-arg PHP_GRPC_LITE_PACKAGE=dkkoma/php-grpc-lite:0.0.2 \
  -t php-grpc-lite-install-pie-0.0.2 .
```

## Enable extension

PIEまたは `make install` が出力したinstall先に合わせて、必要ならPHPの設定へ `extension=grpc` を追加する。

例:

```bash
echo 'extension=grpc' | sudo tee /etc/php/conf.d/20-php-grpc-lite.ini
php -m | grep '^grpc$'
php -r 'var_dump(extension_loaded("grpc"), defined("Grpc\\VERSION") && constant("Grpc\\VERSION") === "0.1.0");'
```

公式 `ext-grpc` が既に有効な環境では、先に公式側の `extension=grpc` 設定を外す。同名moduleなので同時loadはできない。

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
php -r 'require "vendor/autoload.php"; var_dump(extension_loaded("grpc"), defined("Grpc\\VERSION") && constant("Grpc\\VERSION") === "0.1.0", class_exists(Grpc\\Channel::class));'
```

期待値:

- `extension_loaded("grpc") === true`
- `defined("Grpc\\VERSION") && constant("Grpc\\VERSION") === "0.1.0"`
- `class_exists(Grpc\Channel::class) === true`

`extension_loaded("grpc")` だけでは公式 `ext-grpc` と区別できないため、`Grpc\VERSION` の値も確認する。

通常buildでは production bridge のみを公開する。`ext/grpc/bench.php` 用の診断entrypointが必要な場合だけ、開発用途として `./configure --enable-grpc --enable-grpc-bench` でbuildする。

このrepositoryのDocker環境では、HTTP/2 stream lifecycle smokeも確認できる。

```bash
ITERATIONS=10 BENCH_TAG=install-smoke ./tools/test/check-native-lifecycle-stress.sh
```

release hardeningに近い確認をまとめて実行する場合:

```bash
BENCH_TAG=release-hardening ./tools/test/check-native-release-hardening.sh
```

このrunnerは以下を実行する。

- C extension static analysis (`cppcheck`)
- C unit boundary tests
- libFuzzer protocol smoke
- Sanitizer C unit / PHPT (`ASan/UBSan`, `TSan`)
- MSan C core unit
- lifecycle stress smoke
- Valgrind lifecycle smoke (`USE_ZEND_ALLOC=0`, `ZEND_DONT_UNLOAD_MODULES=1`)
- long lifecycle stress
- PHP-FPM request boundaryでのpersistent connection reuse確認

development gateを単独で実行する場合:

```bash
./tools/test/check-native-development-gate.sh
```

Sanitizer / fuzz gateを単独で実行する場合:

```bash
./tools/test/check-c-sanitizer.sh
./tools/test/check-c-msan.sh
./tools/test/check-c-tsan.sh
./tools/test/check-c-fuzz.sh
```

Sanitizer runnerは専用の Clang sanitizer PHP image 上で実行する。ASan/UBSanとTSanはC unitとPHPTを通す。MSanはDebian配布のOpenSSL/nghttp2がMSan instrumentationなしのため、pure C core unitに限定する。Leak検出はValgrind側に寄せるため、ASan leak detectorは無効化する。

native extension test frameworkの全体方針は `docs/native-test-framework.md` を参照する。

release hardeningでSanitizerを一時的に外す場合だけ、`SKIP_SANITIZER=1` を指定する。

## Known limitation

large server streaming bulk transferは事前ベンチを推奨する。目安は `>=64KiB/message` かつ `>=8MiB/stream`、またはlarge payload `>=50 messages`。

この範囲でp99やthroughputがSLOに入るかは、実ワークロードに近いshapeで事前に測定する。
