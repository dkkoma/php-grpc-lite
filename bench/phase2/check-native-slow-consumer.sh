#!/usr/bin/env bash
#
# Compare slow-consumer server-streaming behavior across:
#   1. php-grpc-lite libcurl transport
#   2. php-grpc-lite native stream resource
#   3. official ext-grpc
#
# This runner is not a throughput race. It records the user-visible tradeoff:
# bounded memory/backpressure means total stream time follows consumer speed.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results}"
sleep_us="${SLEEP_US:-1000}"
streams="${STREAMS:-10}"
message_count="${MESSAGE_COUNT:-100}"
payload_bytes="${PAYLOAD_BYTES:-1024}"
mkdir -p "$output_dir"

summary_tsv="$output_dir/phase2-slow-consumer-surface-$timestamp.tsv"

metric() {
    local file="$1"
    local key="$2"
    jq -r --arg key "$key" '.measurements[0].metrics[$key].value // ""' "$file"
}

append_result() {
    local implementation="$1"
    local variant="$2"
    local file="$3"

    local messages wall_ns first_p50_ns stream_p50_ns stream_p99_ns mps usage_max usage_real_max rss_max rss_delta peak_bytes usage_bytes
    messages="$(metric "$file" messages_total)"
    wall_ns="$(metric "$file" wall_time_ns_total)"
    first_p50_ns="$(metric "$file" first_yield_offset_p50_ns)"
    stream_p50_ns="$(metric "$file" stream_latency_p50_ns)"
    stream_p99_ns="$(metric "$file" stream_latency_p99_ns)"
    mps="$(metric "$file" messages_per_second)"
    usage_max="$(metric "$file" memory_usage_max_bytes)"
    usage_real_max="$(metric "$file" memory_usage_real_max_bytes)"
    rss_max="$(metric "$file" diagnostic_rss_max_kib)"
    rss_delta="$(metric "$file" diagnostic_rss_max_delta_kib)"
    peak_bytes="$(metric "$file" memory_peak_delta_bytes)"
    usage_bytes="$(metric "$file" memory_usage_delta_bytes)"

    awk -v implementation="$implementation" \
        -v variant="$variant" \
        -v streams="$streams" \
        -v message_count="$message_count" \
        -v payload_bytes="$payload_bytes" \
        -v sleep_us="$sleep_us" \
        -v messages="$messages" \
        -v wall_ns="$wall_ns" \
        -v first_p50_ns="$first_p50_ns" \
        -v stream_p50_ns="$stream_p50_ns" \
        -v stream_p99_ns="$stream_p99_ns" \
        -v mps="$mps" \
        -v usage_max="$usage_max" \
        -v usage_real_max="$usage_real_max" \
        -v rss_max="$rss_max" \
        -v rss_delta="$rss_delta" \
        -v peak_bytes="$peak_bytes" \
        -v usage_bytes="$usage_bytes" \
        -v file="$file" \
        'BEGIN {
            printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
                implementation, variant, streams, message_count, payload_bytes, sleep_us, messages,
                wall_ns / 1000000.0, first_p50_ns / 1000.0, stream_p50_ns / 1000.0,
                stream_p99_ns / 1000.0, mps, usage_max, usage_real_max, rss_max, rss_delta,
                peak_bytes, usage_bytes, file;
        }' >> "$summary_tsv"
}

run_lite() {
    local variant="$1"
    local transport="$2"
    local extra_php="$3"
    local file="$output_dir/phase2-slow-consumer-surface-$timestamp-$variant.json"

    docker compose run --rm dev sh -lc \
        "php $extra_php tools/phase2/slow-consumer-surface.php --output='$file' --implementation=php-grpc-lite --autoload=vendor/autoload.php --streams=$streams --message-count=$message_count --payload-bytes=$payload_bytes --sleep-us=$sleep_us --transport=$transport --native-response-mode=stream"
    append_result php-grpc-lite "$variant" "$file"
}

run_ext() {
    local file="$output_dir/phase2-slow-consumer-surface-$timestamp-ext-grpc.json"

    docker compose run --rm dev-ext-grpc php tools/phase2/slow-consumer-surface.php \
        --output="$file" \
        --implementation=ext-grpc \
        --autoload=bench-comparison/vendor/autoload.php \
        --streams="$streams" \
        --message-count="$message_count" \
        --payload-bytes="$payload_bytes" \
        --sleep-us="$sleep_us" \
        --transport=curl
    append_result ext-grpc c-core "$file"
}

printf "implementation\tvariant\tstreams\tmessages_per_stream\tpayload_bytes\tsleep_us\tmessages\twall_ms\tfirst_yield_p50_us\tstream_p50_us\tstream_p99_us\tmessages_per_second\tmemory_usage_max_bytes\tmemory_usage_real_max_bytes\tdiagnostic_rss_max_kib\tdiagnostic_rss_max_delta_kib\tmemory_peak_delta_bytes\tmemory_usage_delta_bytes\tjson\n" > "$summary_tsv"

run_lite curl curl ""
run_lite native-stream native "-d extension=/workspace/poc/nghttp2-client-ext/modules/nghttp2_poc.so"
run_ext

echo
echo "Summary TSV: $summary_tsv"
column -t -s $'\t' "$summary_tsv" || cat "$summary_tsv"
