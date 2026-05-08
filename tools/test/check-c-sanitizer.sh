#!/usr/bin/env bash
#
# Build the grpc extension with GCC ASan/UBSan and run focused C unit tests
# plus PHPT integration tests under the sanitizer runtime.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

docker compose run --build --rm dev-sanitizer bash -lc '
    set -euo pipefail

    cd /workspace
    php_bin=/opt/php-sanitizer/bin/php
    phpize_bin=/opt/php-sanitizer/bin/phpize
    php_config_bin=/opt/php-sanitizer/bin/php-config
    test -x "$php_bin" || { echo "sanitizer PHP is not available in the dev-sanitizer image" >&2; exit 127; }
    test -x "$phpize_bin" || { echo "sanitizer phpize is not available in the dev-sanitizer image" >&2; exit 127; }
    test -x "$php_config_bin" || { echo "sanitizer php-config is not available in the dev-sanitizer image" >&2; exit 127; }

    export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1:abort_on_error=1:allocator_may_return_null=1:verify_asan_link_order=0}"
    export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
    export USE_ZEND_ALLOC=0
    export ZEND_DONT_UNLOAD_MODULES=1
    php_sanitized=("$php_bin" -n)

    unit_dir=/workspace/var/coverage/c-sanitizer-unit
    rm -rf "$unit_dir"
    mkdir -p "$unit_dir"
    php_includes="$("$php_config_bin" --includes)"
    pkg_includes="$(pkg-config --cflags libnghttp2 openssl)"

    for test_source in /workspace/ext/grpc/tests/unit/test_*.c; do
        test_name="$(basename "$test_source" .c)"
        cc -D_GNU_SOURCE -std=c99 -Wall -Wextra -Wno-unused-function -Wno-unused-variable \
            -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
            $php_includes $pkg_includes \
            -o "$unit_dir/$test_name" \
            "$test_source" \
            -fsanitize=address,undefined
        "$unit_dir/$test_name"
    done

    cd /workspace/ext/grpc
    make clean >/tmp/grpc-sanitizer-clean.log 2>&1 || true
    rm -rf .libs modules *.lo *.o *.dep
    "$phpize_bin" >/tmp/grpc-sanitizer-phpize.log
    CC=cc \
    CFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined" \
    LDFLAGS="-fsanitize=address,undefined" \
        ./configure --enable-grpc --with-php-config="$php_config_bin" >/tmp/grpc-sanitizer-configure.log
    make -j$(nproc) >/tmp/grpc-sanitizer-make.log

    cd /workspace
    test -f vendor/autoload.php || { echo "vendor/autoload.php is missing; run composer install" >&2; exit 1; }
    "${php_sanitized[@]}" -d extension=/workspace/ext/grpc/modules/grpc.so -r '\''exit(extension_loaded("grpc") ? 0 : 1);'\'' \
        || { echo "grpc extension failed to load from /workspace/ext/grpc/modules/grpc.so" >&2; exit 1; }

    "${php_sanitized[@]}" -r '\''
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
    test_php=/tmp/php-grpc-lite-sanitized-php
    cat > "$test_php" <<'\''SH'\''
#!/usr/bin/env sh
exec /opt/php-sanitizer/bin/php -n "$@"
SH
    chmod +x "$test_php"
    run_tests=/opt/php-sanitizer/lib/php/build/run-tests.php
    test -f "$run_tests" || run_tests=/usr/src/php/run-tests.php

    TEST_PHP_EXECUTABLE="$test_php" \
        "${php_sanitized[@]}" "$run_tests" -q \
        -d extension=/workspace/ext/grpc/modules/grpc.so \
        /workspace/ext/grpc/tests
    cleanup_phpt_artifacts
'
