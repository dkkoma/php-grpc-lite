# php-grpc-lite

[![Native QA](https://github.com/dkkoma/php-grpc-lite/actions/workflows/native-qa.yml/badge.svg)](https://github.com/dkkoma/php-grpc-lite/actions/workflows/native-qa.yml)
[![codecov](https://codecov.io/gh/dkkoma/php-grpc-lite/branch/main/graph/badge.svg)](https://codecov.io/gh/dkkoma/php-grpc-lite)

`php-grpc-lite` is a source-built PHP extension aiming to be a drop-in replacement for official `ext-grpc` in selected client workloads.

Current review status:

- High-level PHP `Grpc\*` wrapper classes are provided by the official `grpc/grpc` Composer package.
- HTTP/2 transport is provided by this repository's source-build extension at the repository root.
- The source-built grpc extension builds a PHP module named `grpc` and produces `grpc.so`.
- Official `ext-grpc` and this source-built grpc extension must not be loaded at the same time.
- Default backend selection is `grpc_lite.backend=http2`: php-grpc-lite uses the built-in nghttp2 HTTP/2 backend unless `franken-go` is explicitly selected.
- Release readiness is still gated by C extension memory/lifecycle QA.
- Unary and server streaming are the current compatibility scope. Client streaming and bidirectional streaming are not implemented yet.

## Performance snapshot

Latest representative local benchmarks: 2026-05-14 for Spanner RPC shape, 2026-05-05 / 2026-05-06 for broader unary and streaming payload sweeps. Environment: Docker compose on OrbStack, Go gRPC test server, `php-grpc-lite` HTTP/2 transport vs official `ext-grpc`. These numbers are workload guidance, not a portability guarantee; rerun the benchmark suite on release hardware for final decisions. Full data: `docs/benchmarks/spanner-shape-2026-05-14.md`, `docs/benchmarks/spanner-real-client-2026-05-14.md`, `docs/benchmarks/native-major-2026-05-05.md`, and `docs/benchmarks/native-hardening-2026-05-06.md`.

| case | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc p50 | ext-grpc p99 | result |
|---|---:|---:|---:|---:|---|
| unary 100B | 28.1μs | 67.8μs | 56.6μs | 103.6μs | php-grpc-lite faster |
| unary 100KiB | 77.9μs | 2,243.1μs | 106.7μs | 1,491.2μs | p50 faster; p99 slower |
| Spanner shape: BeginTransaction unary | 31.6μs | 128.6μs | 59.2μs | 393.4μs | php-grpc-lite faster |
| Spanner shape: Commit unary | 28.2μs | 83.4μs | 54.9μs | 107.0μs | php-grpc-lite faster |
| Spanner shape: SELECT streaming | 28.9μs | 70.9μs | 62.3μs | 115.6μs | php-grpc-lite faster |
| Spanner shape: DML insert streaming | 26.1μs | 69.0μs | 68.1μs | 110.4μs | php-grpc-lite faster |
| Spanner shape: DML update streaming | 26.1μs | 72.5μs | 68.5μs | 110.3μs | php-grpc-lite faster |
| Spanner shape: DML delete streaming | 27.3μs | 65.4μs | 71.1μs | 128.9μs | php-grpc-lite faster |
| server streaming 100x100B | 135.8μs | 538.8μs | 334.1μs | 796.8μs | php-grpc-lite faster |
| server streaming 100x10KiB | 1,111.2μs | 2,255.9μs | 1,452.6μs | 1,974.3μs | p50 faster; p99 slower |

`spanner-shape` is the primary Spanner-oriented performance signal because it keeps the RPC shape close to Spanner while avoiding emulator and GAX noise. `spanner-real-client` is kept as a high-level `google/cloud-spanner` smoke/regression benchmark rather than a transport microbenchmark.

Large bulk streaming remains the main case that should be measured against the actual workload before choosing this extension.

## Install

Install build dependencies first. Debian/Ubuntu example:

```bash
sudo apt-get install -y php-dev build-essential libnghttp2-dev libssl-dev unzip
```

Install the extension with PIE:

```bash
pie install dkkoma/php-grpc-lite --auto-install-build-tools --auto-install-system-dependencies
```

Then enable `extension=grpc` if PIE did not enable it automatically.

PIE uses Composer's default package download path. For this package, Composer prefers the Packagist/GitHub dist zip for the stable release. Ensure a zip extractor is available: `unzip` is the recommended Debian/Ubuntu package; PHP `ext-zip` or `7z` can also satisfy Composer. Without any zip extractor, Composer falls back to source download and may require `git`.

Applications that use generated stubs or gax clients should also install the official PHP wrapper dependency with Composer:

```bash
composer require grpc/grpc
```

For local source builds without PIE:

```bash
git clone <php-grpc-lite repository URL> php-grpc-lite
cd php-grpc-lite
phpize
./configure --enable-grpc
make -j"$(nproc)"
sudo make install
```

Full install notes, verification commands, rollback notes, and large-streaming guidance are in `docs/install-native-extension.md`.

## Backend Selection

The default backend is the built-in HTTP/2 transport:

```ini
grpc_lite.backend=http2
```

The optional FrankenPHP grpc-go backend is explicit opt-in. `auto` is still accepted as a compatibility/testing value: it selects `franken-go` only when an internal `FrankenGrpc\Channel` class is loaded, otherwise it selects `http2`.

Per-channel override:

```php
$channel = new Grpc\Channel($endpoint, [
    'credentials' => Grpc\ChannelCredentials::createInsecure(),
    'grpc_lite.backend' => 'franken-go', // http2 | franken-go | auto
]);
```

The FrankenPHP backend contract and integration notes are in `docs/frankenphp-go-backend-design.md`.

## Development

Run tests in Docker:

```bash
composer install
./tools/test/check-native-development-gate.sh
./tools/test/check-c-unit.sh
./tools/test/check-phpt.sh
./tools/test/check-c-fuzz.sh
./tools/test/check-c-coverage.sh
docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit
```

`check-native-development-gate.sh` runs the normal local gate: C static analysis, C unit boundary tests, PHPT integration tests, and a short libFuzzer smoke. `check-c-unit.sh` runs focused C unit tests for pure protocol helpers and status taxonomy. `check-phpt.sh` builds the root extension, verifies the local Go test-server ports, and runs PHPT tests for the extension surface, transport control semantics, TLS/mTLS, and resource limits. `check-franken-go-backend.sh` smoke-tests the optional FrankenPHP grpc-go backend in the `dev-franken-grpc-go` compose service, whose image fetches `github.com/dkkoma/frankenphp-grpc-go-client` from GitHub by ref. `check-c-fuzz.sh` runs deterministic libFuzzer smoke for pure C protocol boundaries. `check-c-coverage.sh` runs the C unit and PHPT gates with gcov/lcov instrumentation and writes reports under `var/coverage/c-lcov/`. PHPUnit remains the broader integration/release compatibility suite.

GitHub Actions runs the development gate and C coverage gate on push and pull request. Coverage is uploaded as a workflow artifact and to Codecov from `var/coverage/c-lcov/codecov.info`; configure `CODECOV_TOKEN` unless the Codecov repository setting allows tokenless public uploads.

Run static analysis for the C extension:

```bash
./tools/test/check-c-static-analysis.sh
```

Build/load the source-built grpc extension in Docker:

```bash
docker compose run --rm dev sh -lc 'phpize && ./configure --enable-grpc && make -j$(nproc)'
docker compose run --rm dev php -d extension=/workspace/modules/grpc.so -r 'var_dump(extension_loaded("grpc"), defined("Grpc\\VERSION") && constant("Grpc\\VERSION") === "0.1.0");'
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

To verify a specific released package from Packagist:

```bash
docker build -f Dockerfile.install-pie \
  --build-arg PHP_GRPC_LITE_PACKAGE=dkkoma/php-grpc-lite:0.0.2 \
  -t php-grpc-lite-install-pie-0.0.2 .
```

Design and QA status:

- `docs/SPEC.md`
- `docs/http2-transport-decision.md`
- `docs/release-qa-checklist.md`
