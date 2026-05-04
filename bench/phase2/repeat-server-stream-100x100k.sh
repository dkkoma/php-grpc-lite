#!/usr/bin/env bash
#
# Repeat the exceptional 100x100KiB server-streaming shape under the same
# server conditions. This is a focused decision-run input for separating
# transport differences from run-to-run tail variance.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results}"
repeats="${REPEATS:-3}"
streams="${STREAMS:-200}"
message_count=100
payload_bytes=102400
mkdir -p "$output_dir"

summary_tsv="$output_dir/phase2-server-stream-100x100k-repeat-$timestamp.tsv"

metric() {
    local file="$1"
    local key="$2"
    jq -r --arg key "$key" '.measurements[0].metrics[$key].value // ""' "$file"
}

append_result() {
    local repeat="$1"
    local implementation="$2"
    local mode="$3"
    local file="$4"
    local p50_ns p99_ns mps server_p99_ns
    p50_ns="$(metric "$file" stream_latency_p50_ns)"
    p99_ns="$(metric "$file" stream_latency_p99_ns)"
    mps="$(metric "$file" messages_per_second)"
    server_p99_ns="$(metric "$file" server_stats_last_out_payload_ns_p99)"
    awk -v repeat="$repeat" -v implementation="$implementation" -v mode="$mode" -v p50_ns="$p50_ns" -v p99_ns="$p99_ns" -v mps="$mps" -v server_p99_ns="$server_p99_ns" -v file="$file" \
        'BEGIN { printf "%s\t%s\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%s\n", repeat, implementation, mode, p50_ns / 1000.0, p99_ns / 1000.0, mps, server_p99_ns / 1000.0, file }' >> "$summary_tsv"
}

printf "repeat\timplementation\tmode\tp50_us\tp99_us\tmessages_per_second\tserver_last_p99_us\tjson\n" > "$summary_tsv"

for repeat in $(seq 1 "$repeats"); do
    echo
    echo "== repeat $repeat/$repeats: 100x100KiB streams=$streams =="

    libcurl_json="$output_dir/phase2-server-stream-100x100k-repeat-$timestamp-$repeat-libcurl.json"
    docker compose run --rm dev php tools/phase2/streaming-diagnostic.php \
        --suite=streaming-diagnostic \
        --implementation=php-grpc-lite \
        --autoload=vendor/autoload.php \
        --output="$libcurl_json" \
        --streams="$streams" \
        --message-count="$message_count" \
        --payload-bytes="$payload_bytes" \
        --transport=curl
    append_result "$repeat" "php-grpc-lite" "curl" "$libcurl_json"

    native_direct_json="$output_dir/phase2-server-stream-100x100k-repeat-$timestamp-$repeat-native-direct.json"
    docker compose run --rm dev sh -lc \
        "php -d extension=/workspace/ext/grpc/modules/grpc.so tools/phase2/streaming-diagnostic.php --suite=streaming-diagnostic --implementation=php-grpc-lite --autoload=vendor/autoload.php --output='$native_direct_json' --streams=$streams --message-count=$message_count --payload-bytes=$payload_bytes --transport=native --native-transport --native-response-mode=direct"
    append_result "$repeat" "php-grpc-lite" "native-direct" "$native_direct_json"

    native_compact_json="$output_dir/phase2-server-stream-100x100k-repeat-$timestamp-$repeat-native-compact64.json"
    docker compose run --rm dev sh -lc \
        "php -d extension=/workspace/ext/grpc/modules/grpc.so tools/phase2/streaming-diagnostic.php --suite=streaming-diagnostic --implementation=php-grpc-lite --autoload=vendor/autoload.php --output='$native_compact_json' --streams=$streams --message-count=$message_count --payload-bytes=$payload_bytes --transport=native --native-transport --native-response-mode=compact64"
    append_result "$repeat" "php-grpc-lite" "native-compact64" "$native_compact_json"

    ext_json="$output_dir/phase2-server-stream-100x100k-repeat-$timestamp-$repeat-ext-grpc.json"
    docker compose run --rm dev-ext-grpc php tools/phase2/streaming-diagnostic.php \
        --suite=streaming-diagnostic \
        --implementation=ext-grpc \
        --autoload=bench-comparison/vendor/autoload.php \
        --output="$ext_json" \
        --streams="$streams" \
        --message-count="$message_count" \
        --payload-bytes="$payload_bytes"
    append_result "$repeat" "ext-grpc" "c-core" "$ext_json"
done

echo
echo "Summary TSV: $summary_tsv"
column -t -s $'\t' "$summary_tsv" || cat "$summary_tsv"
