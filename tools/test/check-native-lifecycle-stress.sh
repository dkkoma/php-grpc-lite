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
max_fd_delta="${MAX_FD_DELTA:-1}"
max_rss_delta_kib="${MAX_RSS_DELTA_KIB:-}"
max_php_memory_delta_bytes="${MAX_PHP_MEMORY_DELTA_BYTES:-}"
mkdir -p "$output_dir"

json="$output_dir/phase2-native-lifecycle-stress-$timestamp.json"
valgrind_log="$output_dir/phase2-native-lifecycle-stress-$timestamp.valgrind.log"

php_args="-d extension=/workspace/ext/grpc/modules/grpc.so"
tool_args="tools/phase2/native-lifecycle-stress.php --output='$json' --iterations=$iterations --message-count=$message_count --payload-bytes=$payload_bytes --sleep-us=$sleep_us"

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

docker compose run --rm dev php -r '
    $json = $argv[1];
    $maxFdDelta = $argv[2] === "" ? null : (int) $argv[2];
    $maxRssDeltaKiB = $argv[3] === "" ? null : (int) $argv[3];
    $maxPhpMemoryDeltaBytes = $argv[4] === "" ? null : (int) $argv[4];
    $document = json_decode(file_get_contents($json), true, flags: JSON_THROW_ON_ERROR);
    $failures = [];
    foreach ($document["measurements"] ?? [] as $measurement) {
        $name = $measurement["name"] ?? "unknown";
        $metrics = $measurement["metrics"] ?? [];
        $scenarioFailures = (int) ($metrics["failures_total"]["value"] ?? 0);
        $fdDelta = $metrics["fd_count_delta"]["value"] ?? null;
        $rssDelta = $metrics["rss_max_delta_kib"]["value"] ?? null;
        $phpDelta = $metrics["memory_usage_delta_bytes"]["value"] ?? null;
        if ($scenarioFailures !== 0) {
            $failures[] = "$name failures_total=$scenarioFailures";
        }
        if ($maxFdDelta !== null && $fdDelta !== null && (int) $fdDelta > $maxFdDelta) {
            $failures[] = "$name fd_count_delta=$fdDelta > $maxFdDelta";
        }
        if ($maxRssDeltaKiB !== null && $rssDelta !== null && (int) $rssDelta > $maxRssDeltaKiB) {
            $failures[] = "$name rss_max_delta_kib=$rssDelta > $maxRssDeltaKiB";
        }
        if ($maxPhpMemoryDeltaBytes !== null && $phpDelta !== null && (int) $phpDelta > $maxPhpMemoryDeltaBytes) {
            $failures[] = "$name memory_usage_delta_bytes=$phpDelta > $maxPhpMemoryDeltaBytes";
        }
    }
    if ($failures !== []) {
        fwrite(STDERR, "native lifecycle stress failed thresholds:\n- " . implode("\n- ", $failures) . "\n");
        exit(1);
    }
    echo "native lifecycle thresholds OK\n";
' "$json" "$max_fd_delta" "$max_rss_delta_kib" "$max_php_memory_delta_bytes"

echo "JSON: $json"
