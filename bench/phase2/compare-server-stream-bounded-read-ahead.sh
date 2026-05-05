#!/usr/bin/env bash
#
# Sweep bounded read-ahead settings for the nghttp2 server-streaming PoC.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results}"
mkdir -p "$output_dir"

summary_tsv="$output_dir/phase2-server-stream-bounded-read-ahead-$timestamp.tsv"

docker compose run --rm dev sh -lc 'cd ext/grpc && phpize >/tmp/grpc-phpize.log && ./configure --enable-grpc --enable-grpc-bench >/tmp/grpc-configure.log && make -j$(nproc) >/tmp/grpc-make.log'

declare -a cases=(
    "1000x100b 300 1000 100 8388608 32768"
    "10x100k 500 10 102400 8388608 65536"
    "100x100k 200 100 102400 8388608 65536"
    "1x1m 1000 1 1048576 16777216 65536"
    "10000x100b 50 10000 100 8388608 32768"
)

declare -a candidates=(
    "direct --direct-response-payload"
    "read_ahead_unbounded --direct-response-payload --read-ahead-delivery"
    "read_ahead_msg1 --direct-response-payload --read-ahead-delivery --read-ahead-max-messages=1"
    "read_ahead_msg4 --direct-response-payload --read-ahead-delivery --read-ahead-max-messages=4"
    "read_ahead_msg16 --direct-response-payload --read-ahead-delivery --read-ahead-max-messages=16"
    "read_ahead_64k --direct-response-payload --read-ahead-delivery --read-ahead-max-bytes=65536"
    "read_ahead_256k --direct-response-payload --read-ahead-delivery --read-ahead-max-bytes=262144"
    "read_ahead_1m --direct-response-payload --read-ahead-delivery --read-ahead-max-bytes=1048576"
)

poc_json_metric() {
    local file="$1"
    local key="$2"
    jq -r --arg key "$key" '.[$key] // ""' "$file"
}

append_summary() {
    local case_name="$1"
    local candidate="$2"
    local message_count="$3"
    local file="$4"

    local p50 p99 cps server_last_p99_ns poll_wait_p99 queue_count_p99 queue_bytes_p99 queue_wait_p99 payload_string_p99 decode_p99
    p50="$(poc_json_metric "$file" p50_us)"
    p99="$(poc_json_metric "$file" p99_us)"
    cps="$(poc_json_metric "$file" calls_per_second)"
    server_last_p99_ns="$(poc_json_metric "$file" server_stats_last_out_payload_ns_p99)"
    poll_wait_p99="$(poc_json_metric "$file" call_poll_wait_us_p99)"
    queue_count_p99="$(poc_json_metric "$file" call_max_response_queue_count_p99)"
    queue_bytes_p99="$(poc_json_metric "$file" call_max_response_queue_bytes_p99)"
    queue_wait_p99="$(poc_json_metric "$file" call_max_response_queue_wait_us_p99)"
    payload_string_p99="$(poc_json_metric "$file" call_response_payload_string_us_p99)"
    decode_p99="$(poc_json_metric "$file" call_response_decode_us_p99)"

    awk -v case_name="$case_name" \
        -v candidate="$candidate" \
        -v messages="$message_count" \
        -v p50="$p50" \
        -v p99="$p99" \
        -v cps="$cps" \
        -v server_p99_ns="$server_last_p99_ns" \
        -v poll_p99="$poll_wait_p99" \
        -v queue_count_p99="$queue_count_p99" \
        -v queue_bytes_p99="$queue_bytes_p99" \
        -v queue_wait_p99="$queue_wait_p99" \
        -v payload_string_p99="$payload_string_p99" \
        -v decode_p99="$decode_p99" \
        -v file="$file" \
        'BEGIN {
            server_p99 = server_p99_ns == "" ? 0.0 : server_p99_ns / 1000.0;
            residual_p99 = p99 > server_p99 ? p99 - server_p99 : 0.0;
            printf "%s\t%s\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%s\t%s\t%s\t%s\t%s\t%s\n",
                case_name, candidate, messages, p50, p99, cps * messages, server_p99, residual_p99,
                poll_p99, queue_count_p99, queue_bytes_p99, queue_wait_p99, payload_string_p99, decode_p99, file;
        }' >> "$summary_tsv"
}

printf "case\tcandidate\tmessages_per_stream\tp50_us\tp99_us\tmessages_per_second\tserver_last_p99_us\tclient_residual_p99_us\tpoll_wait_p99_us\tqueue_count_p99\tqueue_bytes_p99\tqueue_wait_p99_us\tpayload_string_p99_us\tdecode_p99_us\tjson\n" > "$summary_tsv"

for case_spec in "${cases[@]}"; do
    read -r case_name streams message_count payload_bytes window_size recv_buffer_size <<< "$case_spec"

    echo
    echo "== $case_name: streams=$streams message_count=$message_count payload_bytes=$payload_bytes =="

    for candidate_spec in "${candidates[@]}"; do
        candidate="${candidate_spec%% *}"
        args="${candidate_spec#* }"
        file="$output_dir/phase2-server-stream-bounded-read-ahead-$timestamp-$case_name-$candidate.json"
        docker compose run --rm dev sh -lc \
            "php -d extension=/workspace/ext/grpc/modules/grpc.so /workspace/ext/grpc/bench.php --rpc=server-stream --iterations=$streams --message-count=$message_count --response-bytes=$payload_bytes --split-grpc-frame --no-copy --poll-loop --flush-after-mem-recv --incremental-decode --response-callback-mode=decode-yield --recv-stream-window-size=$window_size --recv-connection-window-size=$window_size --recv-buffer-size=$recv_buffer_size $args" \
            > "$file"
        append_summary "$case_name" "$candidate" "$message_count" "$file"
    done
done

echo
echo "Summary TSV: $summary_tsv"
column -t -s $'\t' "$summary_tsv" || cat "$summary_tsv"
