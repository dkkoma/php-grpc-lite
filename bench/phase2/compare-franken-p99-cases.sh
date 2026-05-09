#!/usr/bin/env bash
#
# Compare the major cases where php-grpc-lite native has historically shown a
# p99 disadvantage against ext-grpc, with FrankenPHP grpc-go as a third axis.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results}"
unary_duration="${UNARY_DURATION:-1}"
stream_duration="${STREAM_DURATION:-1}"
warmup_calls="${WARMUP_CALLS:-3}"
warmup_streams="${WARMUP_STREAMS:-3}"
mkdir -p "$output_dir"

summary_tsv="$output_dir/phase2-franken-p99-cases-$timestamp.tsv"

metric() {
    local file="$1"
    local key="$2"
    jq -r --arg key "$key" '.measurements[0].metrics[$key].value // ""' "$file"
}

append_unary() {
    local implementation="$1"
    local variant="$2"
    local file="$3"

    awk -v implementation="$implementation" \
        -v variant="$variant" \
        -v throughput="$(metric "$file" calls_per_second)" \
        -v p50_ns="$(metric "$file" latency_p50_ns)" \
        -v p99_ns="$(metric "$file" latency_p99_ns)" \
        -v file="$file" \
        'BEGIN {
            printf "unary_100k\t%s\t%s\tcalls/s\t%.1f\t%.1f\t%.1f\t%s\n",
                implementation, variant, throughput, p50_ns / 1000.0, p99_ns / 1000.0, file;
        }' >> "$summary_tsv"
}

append_streaming() {
    local implementation="$1"
    local variant="$2"
    local file="$3"

    awk -v implementation="$implementation" \
        -v variant="$variant" \
        -v throughput="$(metric "$file" messages_per_second)" \
        -v p50_ns="$(metric "$file" stream_latency_p50_ns)" \
        -v p99_ns="$(metric "$file" stream_latency_p99_ns)" \
        -v file="$file" \
        'BEGIN {
            printf "server_streaming_100x10k\t%s\t%s\tmsg/s\t%.1f\t%.1f\t%.1f\t%s\n",
                implementation, variant, throughput, p50_ns / 1000.0, p99_ns / 1000.0, file;
        }' >> "$summary_tsv"
}

printf "case\timplementation\tvariant\tthroughput_unit\tthroughput\tp50_us\tp99_us\tjson\n" > "$summary_tsv"

native_unary_json="$output_dir/phase2-franken-p99-cases-$timestamp-unary-native.json"
docker compose run --rm dev sh -lc \
    "php -d extension=/workspace/ext/grpc/modules/grpc.so tools/phase2/payload-unary.php --suite=franken-p99-cases --implementation=php-grpc-lite --transport=native --autoload=vendor/autoload.php --output='$native_unary_json' --duration='$unary_duration' --payload-sizes=102400 --warmup-calls='$warmup_calls'"
append_unary php-grpc-lite native "$native_unary_json"

franken_unary_json="$output_dir/phase2-franken-p99-cases-$timestamp-unary-franken-go.json"
docker compose run --rm dev-franken-grpc-go tools/frankenphp-grpc-lite-run.sh tools/phase2/payload-unary.php \
    --suite=franken-p99-cases \
    --implementation=php-grpc-lite \
    --transport=franken-go \
    --autoload=vendor/autoload.php \
    --output="$franken_unary_json" \
    --duration="$unary_duration" \
    --payload-sizes=102400 \
    --warmup-calls="$warmup_calls"
append_unary php-grpc-lite franken-go "$franken_unary_json"

ext_unary_json="$output_dir/phase2-franken-p99-cases-$timestamp-unary-ext-grpc.json"
docker compose run --rm dev-ext-grpc php tools/phase2/payload-unary.php \
    --suite=franken-p99-cases \
    --implementation=ext-grpc \
    --autoload=vendor/autoload.php \
    --output="$ext_unary_json" \
    --duration="$unary_duration" \
    --payload-sizes=102400 \
    --warmup-calls="$warmup_calls"
append_unary ext-grpc c-core "$ext_unary_json"

native_stream_json="$output_dir/phase2-franken-p99-cases-$timestamp-stream-native.json"
docker compose run --rm dev sh -lc \
    "php -d extension=/workspace/ext/grpc/modules/grpc.so tools/phase2/throughput-streaming.php --suite=franken-p99-cases --implementation=php-grpc-lite --transport=native --autoload=vendor/autoload.php --output='$native_stream_json' --duration='$stream_duration' --message-count=100 --payload-bytes=10240 --warmup-streams='$warmup_streams'"
append_streaming php-grpc-lite native "$native_stream_json"

franken_stream_json="$output_dir/phase2-franken-p99-cases-$timestamp-stream-franken-go.json"
docker compose run --rm dev-franken-grpc-go tools/frankenphp-grpc-lite-run.sh tools/phase2/throughput-streaming.php \
    --suite=franken-p99-cases \
    --implementation=php-grpc-lite \
    --transport=franken-go \
    --autoload=vendor/autoload.php \
    --output="$franken_stream_json" \
    --duration="$stream_duration" \
    --message-count=100 \
    --payload-bytes=10240 \
    --warmup-streams="$warmup_streams"
append_streaming php-grpc-lite franken-go "$franken_stream_json"

ext_stream_json="$output_dir/phase2-franken-p99-cases-$timestamp-stream-ext-grpc.json"
docker compose run --rm dev-ext-grpc php tools/phase2/throughput-streaming.php \
    --suite=franken-p99-cases \
    --implementation=ext-grpc \
    --autoload=vendor/autoload.php \
    --output="$ext_stream_json" \
    --duration="$stream_duration" \
    --message-count=100 \
    --payload-bytes=10240 \
    --warmup-streams="$warmup_streams"
append_streaming ext-grpc c-core "$ext_stream_json"

echo
echo "Summary TSV: $summary_tsv"
column -t -s $'\t' "$summary_tsv" || cat "$summary_tsv"
