#!/usr/bin/env bash
#
# Run native stream resource lifecycle stress. Set VALGRIND=1 to run the same
# fixture under Valgrind when the dev image contains valgrind.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

iterations="${ITERATIONS:-100}"
message_count="${MESSAGE_COUNT:-20}"
payload_bytes="${PAYLOAD_BYTES:-1024}"
sleep_us="${SLEEP_US:-0}"
valgrind="${VALGRIND:-0}"
valgrind_log="var/bench-results/native-lifecycle-stress-${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}.valgrind.log"
mkdir -p "$(dirname "$valgrind_log")"

php_args="-d extension=/workspace/ext/grpc/modules/grpc.so"
tool_args="tools/benchmark/native-lifecycle-stress.php --iterations=$iterations --message-count=$message_count --payload-bytes=$payload_bytes --sleep-us=$sleep_us"

if [[ "$valgrind" == "1" ]]; then
    docker compose run --rm dev sh -lc "
        cd /workspace/ext/grpc &&
        make clean >/tmp/grpc-lifecycle-clean.log 2>&1 || true &&
        rm -rf .libs modules *.lo *.o *.dep &&
        phpize >/tmp/grpc-lifecycle-phpize.log &&
        ./configure --enable-grpc >/tmp/grpc-lifecycle-configure.log &&
        make -j2 >/tmp/grpc-make.log &&
        cd /workspace &&
        command -v valgrind >/dev/null || { echo 'valgrind is not installed in the dev image' >&2; exit 127; } &&
        USE_ZEND_ALLOC=0 ZEND_DONT_UNLOAD_MODULES=1 \
        valgrind --leak-check=full --show-leak-kinds=definite,indirect,possible --errors-for-leak-kinds=definite,indirect,possible --track-origins=yes --error-exitcode=99 --log-file='$valgrind_log' php $php_args $tool_args
    "
    echo "Valgrind log: $valgrind_log"
else
    docker compose run --rm dev sh -lc "
        cd /workspace/ext/grpc &&
        make clean >/tmp/grpc-lifecycle-clean.log 2>&1 || true &&
        rm -rf .libs modules *.lo *.o *.dep &&
        phpize >/tmp/grpc-lifecycle-phpize.log &&
        ./configure --enable-grpc >/tmp/grpc-lifecycle-configure.log &&
        make -j2 >/tmp/grpc-make.log &&
        cd /workspace &&
        php $php_args $tool_args
    "
fi

echo "native lifecycle stress completed"
