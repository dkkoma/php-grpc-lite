# php-grpc-lite

`php-grpc-lite` is a source-built PHP extension aiming to be a drop-in replacement for official `ext-grpc` in selected client workloads.

Current review status:

- High-level PHP `Grpc\*` wrapper classes are provided by the official `grpc/grpc` Composer package.
- HTTP/2 transport is provided by this repository's source-build extension in `ext/grpc/`.
- The source-built grpc extension builds a PHP module named `grpc` and produces `grpc.so`.
- Official `ext-grpc` and this source-built grpc extension must not be loaded at the same time.
- Runtime transport is nghttp2 only. There is no libcurl fallback and no transport selection option.
- Release readiness is still gated by C extension memory/lifecycle QA.
- Unary and server streaming are the current compatibility scope. Client streaming and bidirectional streaming are not implemented yet.

## Install

Install the extension with PIE:

```bash
pie install dkkoma/php-grpc-lite
```

Then enable `extension=grpc` if PIE did not enable it automatically.

Applications that use generated stubs or gax clients should also install the official PHP wrapper dependency with Composer:

```bash
composer require grpc/grpc
```

For local source builds without PIE:

```bash
git clone <php-grpc-lite repository URL> php-grpc-lite
cd php-grpc-lite/ext/grpc
phpize
./configure --enable-grpc
make -j"$(nproc)"
sudo make install
```

Full install notes, verification commands, rollback notes, and large-streaming guidance are in `docs/install-native-extension.md`.

## Development

Run tests in Docker:

```bash
docker compose run --rm dev php -d extension=/workspace/ext/grpc/modules/grpc.so vendor/bin/phpunit
```

Run static analysis for the C extension:

```bash
./bench/phase2/check-native-static-analysis.sh
```

Build/load the source-built grpc extension in Docker:

```bash
docker compose run --rm dev sh -lc 'cd ext/grpc && phpize && ./configure --enable-grpc && make -j$(nproc)'
docker compose run --rm dev php -d extension=/workspace/ext/grpc/modules/grpc.so -r 'var_dump(extension_loaded("grpc"), defined("Grpc\\VERSION") && constant("Grpc\\VERSION") === "0.1.0");'
```

Verify source install on the official Docker Hub `php` image:

```bash
docker build -f Dockerfile.install-grpc -t php-grpc-lite-install-grpc .
docker run --rm php-grpc-lite-install-grpc php -m | grep -x grpc
docker run --rm php-grpc-lite-install-grpc php -r 'var_dump(extension_loaded("grpc"), defined("Grpc\\VERSION") && constant("Grpc\\VERSION") === "0.1.0");'
```

Verify PIE install on the official Docker Hub `php` image:

```bash
docker build -f Dockerfile.install-pie -t php-grpc-lite-install-pie .
docker run --rm php-grpc-lite-install-pie php -m | grep -x grpc
docker run --rm php-grpc-lite-install-pie php -r 'var_dump(extension_loaded("grpc"), defined("Grpc\\VERSION") && constant("Grpc\\VERSION") === "0.1.0");'
```

Design and QA status:

- `docs/SPEC.md`
- `docs/http2-transport-decision.md`
- `docs/release-qa-checklist.md`
