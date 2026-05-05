# php-grpc-lite

`php-grpc-lite` is a PHP gRPC client implementation aiming to be a drop-in replacement for official `ext-grpc` in selected client workloads.

Current review status:

- PHP userland `Grpc\*` API surface is provided by Composer autoload.
- Native transport is provided by this repository's source-build extension in `ext/grpc/`.
- The native extension builds a PHP module named `grpc` and produces `grpc.so`.
- Official `ext-grpc` and this native extension must not be loaded at the same time.
- Runtime transport is native nghttp2 only. There is no libcurl fallback and no transport selection option.
- Release readiness is still gated by native memory/lifecycle QA.
- Unary and server streaming are the current compatibility scope. Client streaming and bidirectional streaming are not implemented yet.

## Install

Install PHP userland code with Composer:

```bash
composer require php-grpc-lite/php-grpc-lite
```

Build the native extension from this repository:

```bash
git clone <php-grpc-lite repository URL> php-grpc-lite
cd php-grpc-lite/ext/grpc
phpize
./configure --enable-grpc
make -j"$(nproc)"
sudo make install
```

Then enable `extension=grpc.so` in PHP.

Full install notes, verification commands, rollback notes, and large-streaming guidance are in `docs/install-native-extension.md`.

## Development

Run tests in Docker:

```bash
docker compose run --rm dev php -d extension=/workspace/ext/grpc/modules/grpc.so vendor/bin/phpunit
```

Build/load the native extension in Docker:

```bash
docker compose run --rm dev sh -lc 'cd ext/grpc && phpize && ./configure --enable-grpc && make -j$(nproc)'
docker compose run --rm dev php -d extension=/workspace/ext/grpc/modules/grpc.so -r 'var_dump(extension_loaded("grpc"), function_exists("grpc_native_persistent_channel_unary"));'
```

Design and QA status:

- `docs/SPEC.md`
- `docs/native-transport-decision.md`
- `docs/release-qa-checklist.md`
