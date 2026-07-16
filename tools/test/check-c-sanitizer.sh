#!/usr/bin/env bash
#
# Build the grpc extension with a Clang sanitizer. ASan/UBSan and TSan run the
# full PHPT extension suite. MSan runs the C core unit suite only because the
# Debian OpenSSL/nghttp2 dependencies are not built with MSan instrumentation.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

kind="${SANITIZER_KIND:-address-undefined}"
case "$kind" in
    address-undefined) service=dev-sanitizer; run_phpt=1 ;;
    memory) service=dev-msan; run_phpt=0 ;;
    thread) service=dev-tsan; run_phpt=1 ;;
    *)
        echo "unsupported SANITIZER_KIND=$kind" >&2
        exit 2
        ;;
esac

docker compose run --build --rm \
    -e SANITIZER_KIND="$kind" \
    -e RUN_SANITIZER_PHPT="$run_phpt" \
    "$service" bash -lc '
    set -euo pipefail

    cd /workspace
    php_bin=/opt/php-sanitizer/bin/php
    phpize_bin=/opt/php-sanitizer/bin/phpize
    php_config_bin=/opt/php-sanitizer/bin/php-config
    test -x "$php_bin" || { echo "sanitizer PHP is not available in the dev-sanitizer image" >&2; exit 127; }
    test -x "$phpize_bin" || { echo "sanitizer phpize is not available in the dev-sanitizer image" >&2; exit 127; }
    test -x "$php_config_bin" || { echo "sanitizer php-config is not available in the dev-sanitizer image" >&2; exit 127; }

    case "$SANITIZER_KIND" in
        address-undefined)
            sanitizer_flags="-fsanitize=address,undefined"
            export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1:abort_on_error=1:allocator_may_return_null=1:verify_asan_link_order=0}"
            export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
            ;;
        memory)
            sanitizer_flags="-fsanitize=memory -fsanitize-memory-track-origins=2"
            export MSAN_OPTIONS="${MSAN_OPTIONS:-halt_on_error=1:abort_on_error=1:poison_in_dtor=1:track_origins=2}"
            ;;
        thread)
            sanitizer_flags="-fsanitize=thread"
            export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1:abort_on_error=1:report_signal_unsafe=0}"
            ;;
        *)
            echo "unsupported SANITIZER_KIND=$SANITIZER_KIND" >&2
            exit 2
            ;;
    esac

    export USE_ZEND_ALLOC=0
    export ZEND_DONT_UNLOAD_MODULES=1
    php_sanitized=("$php_bin" -n)

    unit_dir=/workspace/var/coverage/c-sanitizer-unit
    rm -rf "$unit_dir"
    mkdir -p "$unit_dir"
    php_includes="$("$php_config_bin" --includes)"
    pkg_includes="$(pkg-config --cflags libnghttp2 openssl)"

    for test_source in /workspace/tests/unit/test_*.c; do
        test_name="$(basename "$test_source" .c)"
        case "$test_name" in
            test_protocol_core) core_source=/workspace/src/protocol_core.c ;;
            test_response_header_phase) core_source=/workspace/src/response_header_phase.c ;;
            test_status_core) core_source=/workspace/src/status_core.c ;;
            test_transport_core) core_source=/workspace/src/transport_core.c ;;
            *) echo "missing core source mapping for $test_name" >&2; exit 1 ;;
        esac
        clang -D_GNU_SOURCE -std=c99 -Wall -Wextra -Wno-unused-function -Wno-unused-variable \
            -O1 -g -fno-omit-frame-pointer $sanitizer_flags \
            -I/workspace -I/workspace/src $php_includes $pkg_includes \
            -o "$unit_dir/$test_name" \
            "$test_source" "$core_source" \
            $sanitizer_flags
        "$unit_dir/$test_name"
    done

    if [[ "${RUN_SANITIZER_PHPT:-1}" != "1" ]]; then
        exit 0
    fi

    cd /workspace
    test -f vendor/autoload.php || { echo "vendor/autoload.php is missing; run composer install" >&2; exit 1; }

    "${php_sanitized[@]}" -r '\''
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

    test_php=/tmp/php-grpc-lite-sanitized-php
    cat > "$test_php" <<'\''SH'\''
#!/usr/bin/env sh
exec /opt/php-sanitizer/bin/php -n "$@"
SH
    chmod +x "$test_php"
    run_tests=/opt/php-sanitizer/lib/php/build/run-tests.php
    test -f "$run_tests" || run_tests=/usr/src/php/run-tests.php

    # Two sanitizer lanes: the production lane runs the exact production
    # binary (no fault seam, no bench surface; fault/bench PHPTs SKIP) so
    # production-only layout/branch memory bugs stay behind the sanitizer
    # gate, and the bench+fault lane runs the fault-injection and bench
    # diagnostic regressions (e.g. PHPT 038-041).
    run_phpt_lane() {
        lane_name="$1"; shift
        expect_bench="$1"; shift
        expect_test_fault="$1"; shift
        echo "=== sanitizer PHPT lane: $lane_name ==="
        cd /workspace
        make clean >"/tmp/grpc-sanitizer-clean-$lane_name.log" 2>&1 || true
        rm -rf .libs modules *.lo *.o *.dep
        "$phpize_bin" >"/tmp/grpc-sanitizer-phpize-$lane_name.log"
        CC=clang \
        CFLAGS="-O1 -g -fno-omit-frame-pointer $sanitizer_flags" \
        LDFLAGS="$sanitizer_flags" \
            ./configure "$@" --with-php-config="$php_config_bin" >"/tmp/grpc-sanitizer-configure-$lane_name.log"
        make -j$(nproc) >"/tmp/grpc-sanitizer-make-$lane_name.log"
        "${php_sanitized[@]}" -d extension=/workspace/modules/grpc.so -r '\''exit(extension_loaded("grpc") ? 0 : 1);'\'' \
            || { echo "grpc extension failed to load from /workspace/modules/grpc.so ($lane_name lane)" >&2; exit 1; }
        cleanup_phpt_artifacts
        GRPC_LITE_EXPECT_BENCH="$expect_bench" \
        GRPC_LITE_EXPECT_TEST_FAULT="$expect_test_fault" \
        TEST_PHP_EXECUTABLE="$test_php" \
            "${php_sanitized[@]}" "$run_tests" -q \
            -d extension=/workspace/modules/grpc.so \
            /workspace/tests/phpt
        cleanup_phpt_artifacts
    }

    run_phpt_lane production 0 0 --enable-grpc
    run_phpt_lane bench-fault 1 1 --enable-grpc --enable-grpc-test-fault --enable-grpc-bench
'
