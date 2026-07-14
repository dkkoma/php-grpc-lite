#!/usr/bin/env bash
#
# Run PHPT tests for the source-built grpc extension with C line coverage.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

docker compose run --rm dev bash -lc '
    set -euo pipefail

    coverage_dir=/workspace/var/coverage/c-lcov
    rm -rf "$coverage_dir"
    mkdir -p "$coverage_dir/html"

    cd /workspace
    make clean >/tmp/grpc-coverage-clean.log 2>&1 || true
    phpize >/tmp/grpc-coverage-phpize.log
    CFLAGS="-O0 -g --coverage" LDFLAGS="--coverage" ./configure --enable-grpc --enable-grpc-test-fault >/tmp/grpc-coverage-configure.log
    make -j$(nproc) >/tmp/grpc-coverage-make.log

    cd /workspace
    test -f vendor/autoload.php || { echo "vendor/autoload.php is missing; run composer install" >&2; exit 1; }
    php -d extension=/workspace/modules/grpc.so -r '\''exit(extension_loaded("grpc") ? 0 : 1);'\'' \
        || { echo "grpc extension failed to load from /workspace/modules/grpc.so" >&2; exit 1; }

    php -r '\''
        foreach ([50051, 50052, 50053, 50054, 50055, 50056, 50057, 50058, 50059, 50060, 50061, 50062, 50063, 50064, 50065, 50066, 50067, 50068, 50069, 50070, 50071] as $port) {
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
        for test_file in tests/phpt/*.phpt; do
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
    for test_source in /workspace/tests/unit/test_*.c; do
        test_name="$(basename "$test_source" .c)"
        case "$test_name" in
            test_protocol_core) core_source=/workspace/src/protocol_core.c ;;
            test_response_header_phase) core_source=/workspace/src/response_header_phase.c ;;
            test_status_core) core_source=/workspace/src/status_core.c ;;
            test_transport_core) core_source=/workspace/src/transport_core.c ;;
            *) echo "missing core source mapping for $test_name" >&2; exit 1 ;;
        esac
        core_name="$(basename "$core_source" .c)"
        cc -D_GNU_SOURCE -std=c99 -Wall -Wextra -Wno-unused-function -Wno-unused-variable -O0 -g --coverage \
            -I/workspace -I/workspace/src $php_includes $pkg_includes \
            -c "$test_source" \
            -o "$c_unit_dir/$test_name.o"
        cc -D_GNU_SOURCE -std=c99 -Wall -Wextra -Wno-unused-function -Wno-unused-variable -O0 -g --coverage \
            -I/workspace -I/workspace/src $php_includes $pkg_includes \
            -c "$core_source" \
            -o "$c_unit_dir/$test_name-$core_name.o"
        cc --coverage \
            "$c_unit_dir/$test_name.o" \
            "$c_unit_dir/$test_name-$core_name.o" \
            -o "$c_unit_dir/$test_name"
        "$c_unit_dir/$test_name" | tee -a "$coverage_dir/c-unit.log"
    done

    GRPC_LITE_EXPECT_TEST_FAULT=1 \
    TEST_PHP_EXECUTABLE="$(command -v php)" \
        php /usr/local/lib/php/build/run-tests.php -q \
        -d extension=/workspace/modules/grpc.so \
        /workspace/tests/phpt | tee "$coverage_dir/phpt.log"

    lcov --capture \
        --directory /workspace \
        --output-file "$coverage_dir/raw.info" \
        --ignore-errors inconsistent \
        >/tmp/grpc-coverage-capture.log
    lcov --extract "$coverage_dir/raw.info" "/workspace/*.c" "/workspace/*.h" "/workspace/src/*.c" "/workspace/src/*.h" "/workspace/src/diagnostic/*.c" "/workspace/src/diagnostic/*.h" \
        --output-file "$coverage_dir/ext-grpc-extracted.info" \
        --ignore-errors unused \
        >/tmp/grpc-coverage-extract.log
    lcov --remove "$coverage_dir/ext-grpc-extracted.info" "/workspace/tests/*" \
        --output-file "$coverage_dir/ext-grpc.info" \
        --ignore-errors unused \
        >/tmp/grpc-coverage-remove-tests.log
    lcov --summary "$coverage_dir/ext-grpc.info" | tee "$coverage_dir/summary.txt"
    sed "s#SF:/workspace/#SF:#" "$coverage_dir/ext-grpc.info" > "$coverage_dir/codecov.info"
    genhtml "$coverage_dir/ext-grpc.info" \
        --output-directory "$coverage_dir/html" \
        --title "php-grpc-lite PHPT C coverage" \
        >/tmp/grpc-coverage-genhtml.log

    cleanup_phpt_artifacts
'
