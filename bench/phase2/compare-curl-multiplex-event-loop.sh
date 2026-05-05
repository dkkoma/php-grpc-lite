#!/usr/bin/env bash
#
# Compare sequential vs shared curl_multi event loop for concurrent
# server-streaming calls. This is a libcurl-level PoC for channel transport
# shape, not a HTTP/2 transport implementation.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results}"
mkdir -p "$output_dir"

summary_tsv="$output_dir/phase2-curl-multiplex-event-loop-$timestamp.tsv"

declare -a cases=(
    "1000x100b 200 1000 100"
    "10x100k 300 10 102400"
    "1x1m 300 1 1048576"
)

declare -a modes=(
    "seq1 1"
    "conc2 2"
    "conc4 4"
    "conc8 8"
)

metric() {
    local file="$1"
    local key="$2"
    jq -r --arg key "$key" '.measurements[0].metrics[$key].value // ""' "$file"
}

append_summary() {
    local case_name="$1"
    local mode="$2"
    local concurrency="$3"
    local file="$4"

    local p50_ns p99_ns mps sps connects ports wall_ns
    p50_ns="$(metric "$file" stream_latency_p50_ns)"
    p99_ns="$(metric "$file" stream_latency_p99_ns)"
    mps="$(metric "$file" messages_per_second)"
    sps="$(metric "$file" streams_per_second)"
    connects="$(metric "$file" curl_num_connects_total)"
    ports="$(metric "$file" curl_local_ports_unique)"
    wall_ns="$(metric "$file" wall_time_ns_total)"

    awk -v case_name="$case_name" \
        -v mode="$mode" \
        -v concurrency="$concurrency" \
        -v p50_ns="$p50_ns" \
        -v p99_ns="$p99_ns" \
        -v mps="$mps" \
        -v sps="$sps" \
        -v connects="$connects" \
        -v ports="$ports" \
        -v wall_ns="$wall_ns" \
        -v file="$file" \
        'BEGIN {
            printf "%s\t%s\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%s\t%s\t%s\n",
                case_name, mode, concurrency, wall_ns / 1000000.0, p50_ns / 1000.0, p99_ns / 1000.0, mps, sps, connects, ports, file;
        }' >> "$summary_tsv"
}

printf "case\tmode\tconcurrency\twall_ms\tp50_us\tp99_us\tmessages_per_second\tstreams_per_second\tcurl_num_connects_total\tcurl_local_ports_unique\tjson\n" > "$summary_tsv"

for case_spec in "${cases[@]}"; do
    read -r case_name streams message_count payload_bytes <<< "$case_spec"

    echo
    echo "== $case_name: streams=$streams message_count=$message_count payload_bytes=$payload_bytes =="

    for mode_spec in "${modes[@]}"; do
        read -r mode concurrency <<< "$mode_spec"
        file="$output_dir/phase2-curl-multiplex-event-loop-$timestamp-$case_name-$mode.json"
        docker compose run --rm dev php tools/phase2/curl-multiplex-streaming.php \
            --suite=curl-multiplex-streaming \
            --implementation=php-curl-multi \
            --autoload=vendor/autoload.php \
            --output="$file" \
            --streams="$streams" \
            --concurrency="$concurrency" \
            --message-count="$message_count" \
            --payload-bytes="$payload_bytes" \
            --multiplex
        append_summary "$case_name" "$mode" "$concurrency" "$file"
    done
done

echo
echo "Summary TSV: $summary_tsv"
column -t -s $'\t' "$summary_tsv" || cat "$summary_tsv"
