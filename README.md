# php-grpc-lite

[![Native QA](https://github.com/dkkoma/php-grpc-lite/actions/workflows/native-qa.yml/badge.svg)](https://github.com/dkkoma/php-grpc-lite/actions/workflows/native-qa.yml)
[![codecov](https://codecov.io/gh/dkkoma/php-grpc-lite/branch/main/graph/badge.svg)](https://codecov.io/gh/dkkoma/php-grpc-lite)

`php-grpc-lite` is a source-built PHP extension aiming to be a drop-in replacement for official `ext-grpc` in selected client workloads.

Current review status:

- High-level PHP `Grpc\*` wrapper classes are provided by the official `grpc/grpc` Composer package.
- HTTP/2 transport is provided by this repository's source-build extension in `ext/grpc/`.
- The source-built grpc extension builds a PHP module named `grpc` and produces `grpc.so`.
- Official `ext-grpc` and this source-built grpc extension must not be loaded at the same time.
- Runtime transport is nghttp2 only. There is no libcurl fallback and no transport selection option.
- Release readiness is still gated by C extension memory/lifecycle QA.
- Unary and server streaming are the current compatibility scope. Client streaming and bidirectional streaming are not implemented yet.

## Performance snapshot

Latest major local benchmarks: 2026-05-05 / 2026-05-06, Docker compose on OrbStack, Go gRPC test server, `php-grpc-lite` HTTP/2 transport vs official `ext-grpc`. These numbers are workload guidance, not a portability guarantee; rerun the benchmark suite on release hardware for final decisions. Full data: `docs/benchmarks/native-major-2026-05-05.md` and `docs/benchmarks/native-hardening-2026-05-06.md`.

| case | php-grpc-lite throughput | php-grpc-lite p50 | php-grpc-lite p99 | ext-grpc throughput | ext-grpc p50 | ext-grpc p99 | result |
|---|---:|---:|---:|---:|---:|---:|---|
| unary 100B | 29,403 calls/s | 28.1μs | 67.8μs | 16,289 calls/s | 56.6μs | 103.6μs | php-grpc-lite faster |
| unary 100KiB | 5,504 calls/s | 77.9μs | 2,243.1μs | 5,458 calls/s | 106.7μs | 1,491.2μs | p50 comparable/faster; p99 slower |
| Spanner DML insert shape | 26,545 calls/s | 31.9μs | 72.5μs | 15,852 calls/s | 58.3μs | 105.8μs | php-grpc-lite faster |
| Spanner commit shape | 26,836 calls/s | 31.8μs | 70.1μs | 15,539 calls/s | 60.6μs | 107.1μs | php-grpc-lite faster |
| Spanner small SELECT 1x1KiB | 12,442 msg/s | 58.0μs | 552.7μs | 6,159 msg/s | 115.3μs | 899.3μs | php-grpc-lite faster |
| Spanner small SELECT 1x10KiB | 9,941 msg/s | 63.4μs | 760.9μs | 5,654 msg/s | 116.6μs | 988.8μs | php-grpc-lite faster |
| server streaming 100x100B | 648,945 msg/s | 135.8μs | 538.8μs | 286,357 msg/s | 334.1μs | 796.8μs | php-grpc-lite faster |
| server streaming 100x10KiB | 78,123 msg/s | 1,111.2μs | 2,255.9μs | 71,098 msg/s | 1,452.6μs | 1,974.3μs | p50/throughput faster; p99 slower |

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
composer install
./tools/test/check-native-development-gate.sh
./tools/test/check-c-unit.sh
./tools/test/check-phpt.sh
./tools/test/check-c-fuzz.sh
./tools/test/check-c-coverage.sh
docker compose run --rm dev php -d extension=/workspace/ext/grpc/modules/grpc.so vendor/bin/phpunit
```

`check-native-development-gate.sh` runs the normal local gate: C static analysis, C unit boundary tests, PHPT integration tests, and a short libFuzzer smoke. `check-c-unit.sh` runs focused C unit tests for pure protocol helpers and status taxonomy. `check-phpt.sh` builds `ext/grpc`, verifies the local Go test-server ports, and runs PHPT tests for the extension surface, transport control semantics, TLS/mTLS, and resource limits. `check-c-fuzz.sh` runs deterministic libFuzzer smoke for pure C protocol boundaries. `check-c-coverage.sh` runs the C unit and PHPT gates with gcov/lcov instrumentation and writes reports under `var/coverage/c-lcov/`. PHPUnit remains the broader integration/release compatibility suite.

GitHub Actions runs the development gate and C coverage gate on push and pull request. Coverage is uploaded as a workflow artifact and to Codecov from `var/coverage/c-lcov/codecov.info`; configure `CODECOV_TOKEN` unless the Codecov repository setting allows tokenless public uploads.

Run static analysis for the C extension:

```bash
./tools/test/check-c-static-analysis.sh
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
