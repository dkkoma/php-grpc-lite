#!/usr/bin/env bash
#
# Run native stream resource lifecycle stress. Set VALGRIND=1 to run the same
# fixture under Valgrind when the dev image contains valgrind.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results}"
iterations="${ITERATIONS:-100}"
message_count="${MESSAGE_COUNT:-20}"
payload_bytes="${PAYLOAD_BYTES:-1024}"
sleep_us="${SLEEP_US:-0}"
valgrind="${VALGRIND:-0}"
mkdir -p "$output_dir"

json="$output_dir/phase2-native-lifecycle-stress-$timestamp.json"
valgrind_log="$output_dir/phase2-native-lifecycle-stress-$timestamp.valgrind.log"

php_args="-d extension=/workspace/ext/grpc/modules/grpc.so"
tool_args="tools/phase2/native-lifecycle-stress.php --output='$json' --iterations=$iterations --message-count=$message_count --payload-bytes=$payload_bytes --sleep-us=$sleep_us"

if [[ "$valgrind" == "1" ]]; then
    docker compose run --rm dev sh -lc "
        cd /workspace/ext/grpc &&
        make -j2 >/tmp/grpc-make.log &&
        cd /workspace &&
        command -v valgrind >/dev/null || { echo 'valgrind is not installed in the dev image' >&2; exit 127; } &&
        valgrind --leak-check=full --show-leak-kinds=definite,indirect,possible --track-origins=yes --error-exitcode=99 --log-file='$valgrind_log' php $php_args $tool_args
    "
    echo "Valgrind log: $valgrind_log"
else
    docker compose run --rm dev sh -lc "
        cd /workspace/ext/grpc &&
        make -j2 >/tmp/grpc-make.log &&
        cd /workspace &&
        php $php_args $tool_args
    "
fi

echo "JSON: $json"
