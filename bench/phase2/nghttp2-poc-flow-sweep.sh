#!/usr/bin/env bash
#
# Run the nghttp2 direct transport PoC with per-call flow-control diagnostics.
#
# This is intentionally separate from the regular Phase 2 php-grpc-lite vs
# ext-grpc runner. The PoC is a transport investigation tool, not a supported
# benchmark implementation.
set -euo pipefail

cd "$(dirname "$0")/../.."

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results/nghttp2-poc-flow-$timestamp}"
iterations="${BENCH_ITERATIONS:-1000}"
request_sizes="${BENCH_REQUEST_PAYLOAD_SIZES:-102400,524288,1048576,2097152}"
response_bytes="${BENCH_RESPONSE_BYTES:-100}"
poc_poll_loop="${BENCH_POC_POLL_LOOP:-0}"
poc_extra_args=()

if [[ "$poc_poll_loop" == "1" ]]; then
    poc_extra_args+=(--poll-loop)
fi

mkdir -p "$output_dir"

docker compose run --rm dev sh -lc 'cd poc/nghttp2-client-ext && phpize >/tmp/nghttp2-poc-phpize.log && ./configure --enable-grpc >/tmp/nghttp2-poc-configure.log && make -j$(nproc) >/tmp/nghttp2-poc-make.log'

IFS=',' read -r -a sizes <<< "$request_sizes"
for size in "${sizes[@]}"; do
    output_path="$output_dir/poc-flow-${size}.json"
    echo "RUN nghttp2 PoC flow sweep: request=${size} output=${output_path}"
    docker compose run --rm dev php \
        -d extension=/workspace/poc/nghttp2-client-ext/modules/grpc.so \
        poc/nghttp2-client-ext/bench.php \
        --iterations="$iterations" \
        --response-bytes="$response_bytes" \
        --request-bytes="$size" \
        --split-grpc-frame \
        --no-copy \
        "${poc_extra_args[@]}" \
        > "$output_path"
done

echo "Saved JSON: $output_dir"
