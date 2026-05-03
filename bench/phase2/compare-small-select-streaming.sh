#!/usr/bin/env bash
#
# Compare small server-streaming response shapes that approximate small SELECT
# results such as Spanner ExecuteStreamingSql returning one PartialResultSet
# message. Multi-message split/chunk behavior is covered by other streaming
# benchmarks.
#
# This runner compares:
#   1. php-grpc-lite libcurl transport
#   2. php-grpc-lite native transport
#   3. official ext-grpc
#   4. native transport PoC variants when INCLUDE_POC=1
#
set -euo pipefail

cd "$(dirname "$0")/../.."

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results}"
native_response_mode="${NATIVE_RESPONSE_MODE:-simple}"
include_poc="${INCLUDE_POC:-1}"
mkdir -p "$output_dir"

summary_tsv="$output_dir/phase2-small-select-streaming-$timestamp.tsv"

declare -a cases=(
    "1x100b 1000 1 100"
    "1x1k 1000 1 1024"
    "1x4k 1000 1 4096"
    "1x10k 1000 1 10240"
)

metric() {
    local file="$1"
    local key="$2"
    jq -r --arg key "$key" '.measurements[0].metrics[$key].value // ""' "$file"
}

poc_metric() {
    local file="$1"
    local key="$2"
    jq -r --arg key "$key" '.[$key] // ""' "$file"
}

append_result() {
    local case_name="$1"
    local implementation="$2"
    local variant="$3"
    local streams="$4"
    local message_count="$5"
    local payload_bytes="$6"
    local file="$7"

    local p50_ns p99_ns mps server_last_p50_ns server_last_p99_ns
    p50_ns="$(metric "$file" stream_latency_p50_ns)"
    p99_ns="$(metric "$file" stream_latency_p99_ns)"
    mps="$(metric "$file" messages_per_second)"
    server_last_p50_ns="$(metric "$file" server_stats_last_out_payload_ns_p50)"
    server_last_p99_ns="$(metric "$file" server_stats_last_out_payload_ns_p99)"

    awk -v case_name="$case_name" \
        -v implementation="$implementation" \
        -v variant="$variant" \
        -v streams="$streams" \
        -v message_count="$message_count" \
        -v payload_bytes="$payload_bytes" \
        -v p50_ns="$p50_ns" \
        -v p99_ns="$p99_ns" \
        -v mps="$mps" \
        -v server_p50_ns="$server_last_p50_ns" \
        -v server_p99_ns="$server_last_p99_ns" \
        -v file="$file" \
        'BEGIN {
            p50 = p50_ns / 1000.0;
            p99 = p99_ns / 1000.0;
            server_p50 = server_p50_ns == "" ? 0.0 : server_p50_ns / 1000.0;
            server_p99 = server_p99_ns == "" ? 0.0 : server_p99_ns / 1000.0;
            residual_p50 = p50 > server_p50 ? p50 - server_p50 : 0.0;
            residual_p99 = p99 > server_p99 ? p99 - server_p99 : 0.0;
            printf "%s\t%s\t%s\t%s\t%s\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%s\n",
                case_name, implementation, variant, streams, message_count, payload_bytes,
                p50, p99, mps, server_p50, server_p99, residual_p50, residual_p99, file;
        }' >> "$summary_tsv"
}

append_poc_result() {
    local case_name="$1"
    local variant="$2"
    local streams="$3"
    local message_count="$4"
    local payload_bytes="$5"
    local file="$6"

    local p50_us p99_us cps server_last_p50_ns server_last_p99_ns
    p50_us="$(poc_metric "$file" p50_us)"
    p99_us="$(poc_metric "$file" p99_us)"
    cps="$(poc_metric "$file" calls_per_second)"
    server_last_p50_ns="$(poc_metric "$file" server_stats_last_out_payload_ns_p50)"
    server_last_p99_ns="$(poc_metric "$file" server_stats_last_out_payload_ns_p99)"

    awk -v case_name="$case_name" \
        -v variant="$variant" \
        -v streams="$streams" \
        -v message_count="$message_count" \
        -v payload_bytes="$payload_bytes" \
        -v p50="$p50_us" \
        -v p99="$p99_us" \
        -v cps="$cps" \
        -v server_p50_ns="$server_last_p50_ns" \
        -v server_p99_ns="$server_last_p99_ns" \
        -v file="$file" \
        'BEGIN {
            server_p50 = server_p50_ns == "" ? 0.0 : server_p50_ns / 1000.0;
            server_p99 = server_p99_ns == "" ? 0.0 : server_p99_ns / 1000.0;
            residual_p50 = p50 > server_p50 ? p50 - server_p50 : 0.0;
            residual_p99 = p99 > server_p99 ? p99 - server_p99 : 0.0;
            printf "%s\t%s\t%s\t%s\t%s\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%s\n",
                case_name, "php-grpc-lite", variant, streams, message_count, payload_bytes,
                p50, p99, cps * message_count, server_p50, server_p99, residual_p50, residual_p99, file;
        }' >> "$summary_tsv"
}

run_streaming_diagnostic() {
    local case_name="$1"
    local service="$2"
    local implementation="$3"
    local variant="$4"
    local streams="$5"
    local message_count="$6"
    local payload_bytes="$7"
    local autoload="$8"
    shift 8

    local file="$output_dir/phase2-small-select-streaming-$timestamp-$case_name-$variant.json"

    docker compose run --rm "$service" php tools/phase2/streaming-diagnostic.php \
        --suite=small-select-streaming \
        --implementation="$implementation" \
        --autoload="$autoload" \
        --output="$file" \
        --streams="$streams" \
        --message-count="$message_count" \
        --payload-bytes="$payload_bytes" \
        "$@"
    append_result "$case_name" "$implementation" "$variant" "$streams" "$message_count" "$payload_bytes" "$file"
}

run_poc() {
    local case_name="$1"
    local variant="$2"
    local streams="$3"
    local message_count="$4"
    local payload_bytes="$5"
    shift 5

    local file="$output_dir/phase2-small-select-streaming-$timestamp-$case_name-$variant.json"

    docker compose run --rm dev sh -lc \
        "php -d extension=/workspace/poc/nghttp2-client-ext/modules/nghttp2_poc.so /workspace/poc/nghttp2-client-ext/bench.php --rpc=server-stream --iterations=$streams --message-count=$message_count --response-bytes=$payload_bytes --split-grpc-frame --no-copy --poll-loop --flush-after-mem-recv --incremental-decode --response-callback-mode=decode-yield --recv-stream-window-size=8388608 --recv-connection-window-size=8388608 --recv-buffer-size=32768 $* " \
        > "$file"
    append_poc_result "$case_name" "$variant" "$streams" "$message_count" "$payload_bytes" "$file"
}

printf "case\timplementation\tvariant\tstreams\tmessages_per_stream\tpayload_bytes\tp50_us\tp99_us\tmessages_per_second\tserver_last_p50_us\tserver_last_p99_us\tclient_residual_p50_us\tclient_residual_p99_us\tjson\n" > "$summary_tsv"

for case_spec in "${cases[@]}"; do
    read -r case_name streams message_count payload_bytes <<< "$case_spec"

    echo
    echo "== $case_name: streams=$streams message_count=$message_count payload_bytes=$payload_bytes =="

    run_streaming_diagnostic "$case_name" dev php-grpc-lite curl \
        "$streams" "$message_count" "$payload_bytes" vendor/autoload.php \
        --transport=curl

    native_file="$output_dir/phase2-small-select-streaming-$timestamp-$case_name-native-$native_response_mode.json"
    docker compose run --rm dev sh -lc \
        "php -d extension=/workspace/poc/nghttp2-client-ext/modules/nghttp2_poc.so tools/phase2/streaming-diagnostic.php --suite=small-select-streaming --implementation=php-grpc-lite --autoload=vendor/autoload.php --output='$native_file' --streams=$streams --message-count=$message_count --payload-bytes=$payload_bytes --transport=native --native-response-mode=$native_response_mode"
    append_result "$case_name" php-grpc-lite "native-$native_response_mode" "$streams" "$message_count" "$payload_bytes" "$native_file"

    run_streaming_diagnostic "$case_name" dev-ext-grpc ext-grpc c-core \
        "$streams" "$message_count" "$payload_bytes" bench-comparison/vendor/autoload.php

    if [[ "$include_poc" == "1" ]]; then
        run_poc "$case_name" poc-compact64 "$streams" "$message_count" "$payload_bytes" \
            --compact-response-buffer --response-compact-threshold=65536
        run_poc "$case_name" poc-direct "$streams" "$message_count" "$payload_bytes" \
            --direct-response-payload
    fi
done

echo
echo "Summary TSV: $summary_tsv"
column -t -s $'\t' "$summary_tsv" || cat "$summary_tsv"
