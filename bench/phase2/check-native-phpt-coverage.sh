#!/usr/bin/env bash
#
# Run PHPT tests for the source-built grpc extension with C line coverage.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

docker compose run --rm dev bash -lc '
    set -euo pipefail

    coverage_dir=/workspace/var/coverage/phpt-lcov
    rm -rf "$coverage_dir"
    mkdir -p "$coverage_dir/html"

    cd /workspace/ext/grpc
    make clean >/tmp/grpc-coverage-clean.log 2>&1 || true
    phpize >/tmp/grpc-coverage-phpize.log
    CFLAGS="-O0 -g --coverage" LDFLAGS="--coverage" ./configure --enable-grpc >/tmp/grpc-coverage-configure.log
    make -j$(nproc) >/tmp/grpc-coverage-make.log

    cd /workspace
    test -f vendor/autoload.php || { echo "vendor/autoload.php is missing; run composer install" >&2; exit 1; }
    php -d extension=/workspace/ext/grpc/modules/grpc.so -r '\''exit(extension_loaded("grpc") ? 0 : 1);'\'' \
        || { echo "grpc extension failed to load from /workspace/ext/grpc/modules/grpc.so" >&2; exit 1; }

    php -r '\''
        foreach ([50051, 50052, 50053, 50054, 50055, 50056, 50057, 50058, 50059, 50060] as $port) {
            $connected = false;
            $lastError = "";
            for ($attempt = 1; $attempt <= 30; $attempt++) {
                $socket = @fsockopen("test-server", $port, $errno, $errstr, 1.0);
                if (is_resource($socket)) {
                    fclose($socket);
                    $connected = true;
                    break;
                }
                $lastError = $errstr;
                usleep(100000);
            }
            if (!$connected) {
                fwrite(STDERR, "test-server:$port is not reachable after retry: $lastError\n");
                exit(1);
            }
        }
    '\''

    cleanup_phpt_artifacts() {
        local test_file artifact_base
        for test_file in ext/grpc/tests/*.phpt; do
            artifact_base="${test_file%.phpt}"
            rm -f \
                "${artifact_base}.log" \
                "${artifact_base}.out" \
                "${artifact_base}.diff" \
                "${artifact_base}.exp" \
                "${artifact_base}.php" \
                "${artifact_base}.sh"
        done
    }

    cleanup_phpt_artifacts
    lcov --zerocounters --directory /workspace >/tmp/grpc-coverage-zero.log

    c_unit_dir="$coverage_dir/c-unit"
    mkdir -p "$c_unit_dir"
    php_includes="$(php-config --includes)"
    pkg_includes="$(pkg-config --cflags libnghttp2 openssl)"
    : > "$coverage_dir/c-unit.log"
    for test_source in /workspace/ext/grpc/tests/unit/test_*.c; do
        test_name="$(basename "$test_source" .c)"
        cc -D_GNU_SOURCE -std=c99 -Wall -Wextra -Wno-unused-function -Wno-unused-variable -O0 -g --coverage \
            $php_includes $pkg_includes \
            -c "$test_source" \
            -o "$c_unit_dir/$test_name.o"
        cc --coverage \
            "$c_unit_dir/$test_name.o" \
            -o "$c_unit_dir/$test_name"
        "$c_unit_dir/$test_name" | tee -a "$coverage_dir/c-unit.log"
    done

    TEST_PHP_EXECUTABLE="$(command -v php)" \
        php /usr/local/lib/php/build/run-tests.php -q \
        -d extension=/workspace/ext/grpc/modules/grpc.so \
        /workspace/ext/grpc/tests | tee "$coverage_dir/phpt.log"

    lcov --capture \
        --directory /workspace \
        --output-file "$coverage_dir/raw.info" \
        --ignore-errors inconsistent \
        >/tmp/grpc-coverage-capture.log
    lcov --extract "$coverage_dir/raw.info" "/workspace/ext/grpc/*" \
        --output-file "$coverage_dir/ext-grpc-extracted.info" \
        --ignore-errors unused \
        >/tmp/grpc-coverage-extract.log
    lcov --remove "$coverage_dir/ext-grpc-extracted.info" "/workspace/ext/grpc/tests/*" \
        --output-file "$coverage_dir/ext-grpc.info" \
        --ignore-errors unused \
        >/tmp/grpc-coverage-remove-tests.log
    lcov --summary "$coverage_dir/ext-grpc.info" | tee "$coverage_dir/summary.txt"
    genhtml "$coverage_dir/ext-grpc.info" \
        --output-directory "$coverage_dir/html" \
        --title "php-grpc-lite PHPT C coverage" \
        >/tmp/grpc-coverage-genhtml.log

    cleanup_phpt_artifacts
'
