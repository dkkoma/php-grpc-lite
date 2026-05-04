#!/usr/bin/env bash
#
# Compare the three Phase 2 transport lines:
#   1. current php-grpc-lite libcurl transport
#   2. nghttp2 native MVP extension PoC
#   3. official ext-grpc
#
set -euo pipefail

cd "$(dirname "$0")/../.."

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results}"
mkdir -p "$output_dir"

summary_tsv="$output_dir/phase2-native-mvp-vs-libcurl-ext-$timestamp.tsv"

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

append_unary_result() {
    local implementation="$1"
    local file="$2"
    local p50_ns p99_ns cps
    p50_ns="$(metric "$file" latency_p50_ns)"
    p99_ns="$(metric "$file" latency_p99_ns)"
    cps="$(metric "$file" calls_per_second)"
    awk -v implementation="$implementation" -v p50_ns="$p50_ns" -v p99_ns="$p99_ns" -v cps="$cps" -v file="$file" \
        'BEGIN { printf "large-request-unary\t%s\tsurface\t%.1f\t%.1f\t%.1f\t\t\t%s\n", implementation, p50_ns / 1000.0, p99_ns / 1000.0, cps, file }' >> "$summary_tsv"
}

append_unary_poc_result() {
    local implementation="$1"
    local file="$2"
    local p50 p99 cps
    p50="$(poc_metric "$file" p50_us)"
    p99="$(poc_metric "$file" p99_us)"
    cps="$(poc_metric "$file" calls_per_second)"
    awk -v implementation="$implementation" -v p50="$p50" -v p99="$p99" -v cps="$cps" -v file="$file" \
        'BEGIN { printf "large-request-unary\t%s\tpoc\t%.1f\t%.1f\t%.1f\t\t\t%s\n", implementation, p50, p99, cps, file }' >> "$summary_tsv"
}

append_stream_result() {
    local case_name="$1"
    local implementation="$2"
    local file="$3"
    local p50_ns p99_ns mps server_p99_ns
    p50_ns="$(metric "$file" stream_latency_p50_ns)"
    p99_ns="$(metric "$file" stream_latency_p99_ns)"
    mps="$(metric "$file" messages_per_second)"
    server_p99_ns="$(metric "$file" server_stats_last_out_payload_ns_p99)"
    awk -v case_name="$case_name" -v implementation="$implementation" -v p50_ns="$p50_ns" -v p99_ns="$p99_ns" -v mps="$mps" -v server_p99_ns="$server_p99_ns" -v file="$file" \
        'BEGIN { printf "%s\t%s\tsurface\t%.1f\t%.1f\t%.1f\t%.1f\t\t%s\n", case_name, implementation, p50_ns / 1000.0, p99_ns / 1000.0, mps, server_p99_ns / 1000.0, file }' >> "$summary_tsv"
}

append_stream_poc_result() {
    local case_name="$1"
    local implementation="$2"
    local file="$3"
    local message_count="$4"
    local p50 p99 cps server_p99_ns poll_p99 max_buffer_p99
    p50="$(poc_metric "$file" p50_us)"
    p99="$(poc_metric "$file" p99_us)"
    cps="$(poc_metric "$file" calls_per_second)"
    server_p99_ns="$(poc_metric "$file" server_stats_last_out_payload_ns_p99)"
    poll_p99="$(poc_metric "$file" call_poll_wait_us_p99)"
    max_buffer_p99="$(poc_metric "$file" call_max_body_buffer_bytes_p99)"
    awk -v case_name="$case_name" -v implementation="$implementation" -v p50="$p50" -v p99="$p99" -v cps="$cps" -v message_count="$message_count" -v server_p99_ns="$server_p99_ns" -v poll_p99="$poll_p99" -v max_buffer_p99="$max_buffer_p99" -v file="$file" \
        'BEGIN { printf "%s\t%s\tpoc\t%.1f\t%.1f\t%.1f\t%.1f\t%s\t%s\t%s\n", case_name, implementation, p50, p99, cps * message_count, server_p99_ns / 1000.0, poll_p99, max_buffer_p99, file }' >> "$summary_tsv"
}

printf "case\timplementation\tvariant\tp50_us\tp99_us\tthroughput\tserver_last_p99_us\tpoll_wait_p99_us\tmax_body_buffer_bytes_p99\tjson\n" > "$summary_tsv"

echo "== large-request-unary: 1MiB request / 100B response =="
libcurl_unary="$output_dir/phase2-native-mvp-vs-libcurl-ext-$timestamp-large-request-unary-libcurl.json"
docker compose run --rm dev php tools/phase2/request-unary.php \
    --suite=request-unary \
    --implementation=php-grpc-lite \
    --autoload=vendor/autoload.php \
    --output="$libcurl_unary" \
    --duration=2 \
    --request-payload-sizes=1048576 \
    --warmup-calls=3 \
    --max-calls=1000 \
    --transport=curl
append_unary_result "libcurl" "$libcurl_unary"

mvp_unary="$output_dir/phase2-native-mvp-vs-libcurl-ext-$timestamp-large-request-unary-mvp-upload.json"
docker compose run --rm dev sh -lc \
    "php -d extension=/workspace/ext/grpc/modules/grpc.so /workspace/ext/grpc/bench.php --rpc=unary --iterations=1000 --request-bytes=1048576 --response-bytes=100 --split-grpc-frame --no-copy --poll-loop" \
    > "$mvp_unary"
append_unary_poc_result "mvp-upload" "$mvp_unary"

native_surface_unary="$output_dir/phase2-native-mvp-vs-libcurl-ext-$timestamp-large-request-unary-native-surface.json"
docker compose run --rm dev sh -lc \
    "php -d extension=/workspace/ext/grpc/modules/grpc.so tools/phase2/request-unary.php --suite=request-unary --implementation=php-grpc-lite --autoload=vendor/autoload.php --output='$native_surface_unary' --duration=2 --request-payload-sizes=1048576 --warmup-calls=3 --max-calls=1000 --transport=native"
append_unary_result "native-surface" "$native_surface_unary"

ext_unary="$output_dir/phase2-native-mvp-vs-libcurl-ext-$timestamp-large-request-unary-ext-grpc.json"
docker compose run --rm dev-ext-grpc php tools/phase2/request-unary.php \
    --suite=request-unary \
    --implementation=ext-grpc \
    --autoload=bench-comparison/vendor/autoload.php \
    --output="$ext_unary" \
    --duration=2 \
    --request-payload-sizes=1048576 \
    --warmup-calls=3 \
    --max-calls=1000
append_unary_result "ext-grpc" "$ext_unary"

declare -a stream_cases=(
    "1000x100b 300 1000 100 8388608 32768"
    "10x100k 500 10 102400 8388608 65536"
    "100x100k 200 100 102400 8388608 65536"
    "1x1m 1000 1 1048576 16777216 65536"
    "10000x100b 50 10000 100 8388608 32768"
)

for case_spec in "${stream_cases[@]}"; do
    read -r case_name streams message_count payload_bytes window_size recv_buffer_size <<< "$case_spec"
    echo
    echo "== $case_name: streams=$streams message_count=$message_count payload_bytes=$payload_bytes =="

    libcurl_stream="$output_dir/phase2-native-mvp-vs-libcurl-ext-$timestamp-$case_name-libcurl.json"
    docker compose run --rm dev php tools/phase2/streaming-diagnostic.php \
        --suite=streaming-diagnostic \
        --implementation=php-grpc-lite \
        --autoload=vendor/autoload.php \
        --output="$libcurl_stream" \
        --streams="$streams" \
        --message-count="$message_count" \
        --payload-bytes="$payload_bytes" \
        --transport=curl
    append_stream_result "$case_name" "libcurl" "$libcurl_stream"

    mvp_direct="$output_dir/phase2-native-mvp-vs-libcurl-ext-$timestamp-$case_name-mvp-direct.json"
    docker compose run --rm dev sh -lc \
        "php -d extension=/workspace/ext/grpc/modules/grpc.so /workspace/ext/grpc/bench.php --rpc=server-stream --iterations=$streams --message-count=$message_count --response-bytes=$payload_bytes --split-grpc-frame --no-copy --poll-loop --flush-after-mem-recv --incremental-decode --response-callback-mode=decode-yield --recv-stream-window-size=$window_size --recv-connection-window-size=$window_size --recv-buffer-size=$recv_buffer_size --direct-response-payload" \
        > "$mvp_direct"
    append_stream_poc_result "$case_name" "mvp-direct" "$mvp_direct" "$message_count"

    native_surface_stream="$output_dir/phase2-native-mvp-vs-libcurl-ext-$timestamp-$case_name-native-surface-stream.json"
    docker compose run --rm dev sh -lc \
        "php -d extension=/workspace/ext/grpc/modules/grpc.so tools/phase2/streaming-diagnostic.php --suite=streaming-diagnostic --implementation=php-grpc-lite --autoload=vendor/autoload.php --output='$native_surface_stream' --streams=$streams --message-count=$message_count --payload-bytes=$payload_bytes --transport=native --native-transport --native-response-mode=stream"
    append_stream_result "$case_name" "native-surface-stream" "$native_surface_stream"

    mvp_compact="$output_dir/phase2-native-mvp-vs-libcurl-ext-$timestamp-$case_name-mvp-compact64.json"
    docker compose run --rm dev sh -lc \
        "php -d extension=/workspace/ext/grpc/modules/grpc.so /workspace/ext/grpc/bench.php --rpc=server-stream --iterations=$streams --message-count=$message_count --response-bytes=$payload_bytes --split-grpc-frame --no-copy --poll-loop --flush-after-mem-recv --incremental-decode --response-callback-mode=decode-yield --recv-stream-window-size=$window_size --recv-connection-window-size=$window_size --recv-buffer-size=$recv_buffer_size --compact-response-buffer --response-compact-threshold=65536" \
        > "$mvp_compact"
    append_stream_poc_result "$case_name" "mvp-compact64" "$mvp_compact" "$message_count"

    ext_stream="$output_dir/phase2-native-mvp-vs-libcurl-ext-$timestamp-$case_name-ext-grpc.json"
    docker compose run --rm dev-ext-grpc php tools/phase2/streaming-diagnostic.php \
        --suite=streaming-diagnostic \
        --implementation=ext-grpc \
        --autoload=bench-comparison/vendor/autoload.php \
        --output="$ext_stream" \
        --streams="$streams" \
        --message-count="$message_count" \
        --payload-bytes="$payload_bytes"
    append_stream_result "$case_name" "ext-grpc" "$ext_stream"
done

echo
echo "Summary TSV: $summary_tsv"
column -t -s $'\t' "$summary_tsv" || cat "$summary_tsv"
