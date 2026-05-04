#!/usr/bin/env bash
#
# Compare server-streaming large-response transport candidates against
# php-grpc-lite and ext-grpc with the same BenchServerStream shapes.
#
# This is an exploratory Phase 2 runner. It is not part of the regression
# baseline because it depends on the nghttp2 transport PoC extension.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results}"
mkdir -p "$output_dir"

summary_tsv="$output_dir/phase2-server-stream-poc-goal-$timestamp.tsv"

declare -a cases=(
    "1000x100b 300 1000 100 8388608 32768"
    "10x100k 500 10 102400 8388608 65536"
    "100x100k 200 100 102400 8388608 65536"
    "1x1m 1000 1 1048576 16777216 65536"
    "10000x100b 50 10000 100 8388608 32768"
)

phase2_json_metric() {
    local file="$1"
    local key="$2"
    jq -r --arg key "$key" '.measurements[0].metrics[$key].value // ""' "$file"
}

poc_json_metric() {
    local file="$1"
    local key="$2"
    jq -r --arg key "$key" '.[$key] // ""' "$file"
}

append_phase2_summary() {
    local case_name="$1"
    local implementation="$2"
    local file="$3"
    local message_count="$4"

    local p50_ns p99_ns mps server_last_p99_ns server_last_p50_ns
    p50_ns="$(phase2_json_metric "$file" stream_latency_p50_ns)"
    p99_ns="$(phase2_json_metric "$file" stream_latency_p99_ns)"
    mps="$(phase2_json_metric "$file" messages_per_second)"
    server_last_p50_ns="$(phase2_json_metric "$file" server_stats_last_out_payload_ns_p50)"
    server_last_p99_ns="$(phase2_json_metric "$file" server_stats_last_out_payload_ns_p99)"

    awk -v case_name="$case_name" \
        -v implementation="$implementation" \
        -v messages="$message_count" \
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
            printf "%s\t%s\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t\t\t%s\n",
                case_name, implementation, messages, p50, p99, mps, server_p50, server_p99, residual_p50, residual_p99, file;
        }' >> "$summary_tsv"
}

append_poc_summary() {
    local case_name="$1"
    local implementation="$2"
    local file="$3"
    local message_count="$4"

    local p50_us p99_us cps server_last_p50_ns server_last_p99_ns poll_wait_p99_us max_buffer_p99 payload_string_p99
    p50_us="$(poc_json_metric "$file" p50_us)"
    p99_us="$(poc_json_metric "$file" p99_us)"
    cps="$(poc_json_metric "$file" calls_per_second)"
    server_last_p50_ns="$(poc_json_metric "$file" server_stats_last_out_payload_ns_p50)"
    server_last_p99_ns="$(poc_json_metric "$file" server_stats_last_out_payload_ns_p99)"
    poll_wait_p99_us="$(poc_json_metric "$file" call_poll_wait_us_p99)"
    max_buffer_p99="$(poc_json_metric "$file" call_max_body_buffer_bytes_p99)"
    payload_string_p99="$(poc_json_metric "$file" call_response_payload_string_us_p99)"

    awk -v case_name="$case_name" \
        -v implementation="$implementation" \
        -v messages="$message_count" \
        -v p50="$p50_us" \
        -v p99="$p99_us" \
        -v cps="$cps" \
        -v server_p50_ns="$server_last_p50_ns" \
        -v server_p99_ns="$server_last_p99_ns" \
        -v poll_p99="$poll_wait_p99_us" \
        -v max_buffer_p99="$max_buffer_p99" \
        -v payload_string_p99="$payload_string_p99" \
        -v file="$file" \
        'BEGIN {
            server_p50 = server_p50_ns == "" ? 0.0 : server_p50_ns / 1000.0;
            server_p99 = server_p99_ns == "" ? 0.0 : server_p99_ns / 1000.0;
            residual_p50 = p50 > server_p50 ? p50 - server_p50 : 0.0;
            residual_p99 = p99 > server_p99 ? p99 - server_p99 : 0.0;
            printf "%s\t%s\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%s\t%s\t%s\t%s\n",
                case_name, implementation, messages, p50, p99, cps * messages, server_p50, server_p99, residual_p50, residual_p99, poll_p99, max_buffer_p99, payload_string_p99, file;
        }' >> "$summary_tsv"
}

run_streaming_diagnostic() {
    local case_name="$1"
    local service="$2"
    local implementation="$3"
    local streams="$4"
    local message_count="$5"
    local payload_bytes="$6"
    local autoload="$7"
    local file="$output_dir/phase2-server-stream-poc-goal-$timestamp-$case_name-$implementation.json"

    docker compose run --rm "$service" php tools/phase2/streaming-diagnostic.php \
        --suite=streaming-diagnostic \
        --implementation="$implementation" \
        --autoload="$autoload" \
        --output="$file" \
        --streams="$streams" \
        --message-count="$message_count" \
        --payload-bytes="$payload_bytes"
    append_phase2_summary "$case_name" "$implementation" "$file" "$message_count"
}

run_poc() {
    local case_name="$1"
    local implementation="$2"
    local streams="$3"
    local message_count="$4"
    local payload_bytes="$5"
    local window_size="$6"
    local recv_buffer_size="$7"
    shift 7
    local extra_args=("$@")
    local file="$output_dir/phase2-server-stream-poc-goal-$timestamp-$case_name-$implementation.json"

    docker compose run --rm dev sh -lc \
        "php -d extension=/workspace/poc/nghttp2-client-ext/modules/grpc.so /workspace/poc/nghttp2-client-ext/bench.php --rpc=server-stream --iterations=$streams --message-count=$message_count --response-bytes=$payload_bytes --split-grpc-frame --no-copy --poll-loop --flush-after-mem-recv --incremental-decode --response-callback-mode=decode-yield --recv-stream-window-size=$window_size --recv-connection-window-size=$window_size --recv-buffer-size=$recv_buffer_size ${extra_args[*]}" \
        > "$file"
    append_poc_summary "$case_name" "$implementation" "$file" "$message_count"
}

printf "case\timplementation\tmessages_per_stream\tp50_us\tp99_us\tmessages_per_second\tserver_last_p50_us\tserver_last_p99_us\tclient_residual_p50_us\tclient_residual_p99_us\tpoll_wait_p99_us\tmax_body_buffer_bytes_p99\tpayload_string_p99_us\tjson\n" > "$summary_tsv"

for case_spec in "${cases[@]}"; do
    read -r case_name streams message_count payload_bytes window_size recv_buffer_size <<< "$case_spec"

    echo
    echo "== $case_name: streams=$streams message_count=$message_count payload_bytes=$payload_bytes =="

    run_streaming_diagnostic "$case_name" dev php-grpc-lite "$streams" "$message_count" "$payload_bytes" vendor/autoload.php
    run_streaming_diagnostic "$case_name" dev-ext-grpc ext-grpc "$streams" "$message_count" "$payload_bytes" bench-comparison/vendor/autoload.php

    run_poc "$case_name" poc-direct "$streams" "$message_count" "$payload_bytes" "$window_size" "$recv_buffer_size" --direct-response-payload
    run_poc "$case_name" poc-compact64 "$streams" "$message_count" "$payload_bytes" "$window_size" "$recv_buffer_size" --compact-response-buffer --response-compact-threshold=65536
done

echo
echo "Summary TSV: $summary_tsv"
column -t -s $'\t' "$summary_tsv" || cat "$summary_tsv"
