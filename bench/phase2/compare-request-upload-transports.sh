#!/usr/bin/env bash
#
# Compare large request / small response upload behavior across:
#   - php-grpc-lite
#   - official ext-grpc
#   - nghttp2 direct transport PoC
#
# All runs use the same docker compose test-server instance. Use environment
# variables such as TEST_SERVER_GOMAXPROCS or
# TEST_SERVER_GRPC_INITIAL_CONN_WINDOW_SIZE before invoking this script to test
# scheduler / HTTP/2 window conditions.
# Set BENCH_POC_POLL_LOOP=1 to run the nghttp2 PoC with no-copy DATA and the
# nonblocking poll loop.
set -euo pipefail

cd "$(dirname "$0")/../.."

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results/request-upload-transports-$timestamp}"
request_sizes="${BENCH_REQUEST_PAYLOAD_SIZES:-102400,524288,1048576,2097152}"
max_calls="${BENCH_MAX_CALLS:-1000}"
duration="${BENCH_DURATION:-30}"
warmup_calls="${BENCH_WARMUP_CALLS:-10}"
poc_iterations="${BENCH_POC_ITERATIONS:-$max_calls}"
response_bytes="${BENCH_RESPONSE_BYTES:-100}"
poc_poll_loop="${BENCH_POC_POLL_LOOP:-0}"
poc_label="poc-flow"
poc_extra_args=()

if [[ "$poc_poll_loop" == "1" ]]; then
    poc_label="poc-flow-poll"
    poc_extra_args+=(--poll-loop)
fi

mkdir -p "$output_dir"

if [[ "${BENCH_RECREATE_TEST_SERVER:-1}" == "1" ]]; then
    docker compose up -d --force-recreate test-server
else
    docker compose up -d test-server
fi

docker compose run --rm dev sh -lc 'cd poc/nghttp2-client-ext && phpize >/tmp/nghttp2-poc-phpize.log && ./configure --enable-grpc >/tmp/nghttp2-poc-configure.log && make -j$(nproc) >/tmp/nghttp2-poc-make.log'

BENCH_TAG="${timestamp}-php" \
BENCH_IMPLEMENTATION=php-grpc-lite \
BENCH_OUTPUT_DIR="$output_dir" \
    ./bench/phase2/run.sh request-unary-diagnostic \
        --duration="$duration" \
        --max-calls="$max_calls" \
        --request-payload-sizes="$request_sizes" \
        --warmup-calls="$warmup_calls"

BENCH_TAG="${timestamp}-ext" \
BENCH_IMPLEMENTATION=ext-grpc \
BENCH_OUTPUT_DIR="$output_dir" \
    ./bench/phase2/run.sh request-unary-diagnostic \
        --duration="$duration" \
        --max-calls="$max_calls" \
        --request-payload-sizes="$request_sizes" \
        --warmup-calls="$warmup_calls"

IFS=',' read -r -a sizes <<< "$request_sizes"
for size in "${sizes[@]}"; do
    output_path="$output_dir/${poc_label}-${timestamp}-${size}.json"
    echo "RUN nghttp2 PoC flow sweep: request=${size} output=${output_path}"
    docker compose run --rm dev php \
        -d extension=/workspace/poc/nghttp2-client-ext/modules/grpc.so \
        poc/nghttp2-client-ext/bench.php \
        --iterations="$poc_iterations" \
        --response-bytes="$response_bytes" \
        --request-bytes="$size" \
        --split-grpc-frame \
        --no-copy \
        "${poc_extra_args[@]}" \
        > "$output_path"
done

echo "Saved JSON: $output_dir"
