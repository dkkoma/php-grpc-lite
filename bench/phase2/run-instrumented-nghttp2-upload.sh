#!/usr/bin/env bash
#
# Run request-unary diagnostics through instrumented nghttp2 + libcurl.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

output_dir="${BENCH_OUTPUT_DIR:-var/bench-results/instrumented-nghttp2-$(date +%Y%m%d-%H%M%S)}"
payload_bytes="${BENCH_PAYLOAD_BYTES:-1048576}"
max_calls="${BENCH_MAX_CALLS:-3}"
duration_sec="${BENCH_DURATION:-1}"
ld_path="/workspace/var/instrumented-curl-nghttp2/lib:/workspace/var/instrumented-nghttp2/lib"

if [[ ! -f var/instrumented-curl-nghttp2/lib/libcurl.so.4 || ! -f var/instrumented-nghttp2/lib/libnghttp2.so.14 ]]; then
    cat >&2 <<EOF
Missing instrumented libraries.

Build them first:
  ./bench/phase2/build-instrumented-nghttp2-libcurl.sh
EOF
    exit 2
fi

mkdir -p "$output_dir"

run_case() {
    local name="$1"
    shift

    docker compose run --rm dev sh -lc "
set -euo pipefail
export LD_LIBRARY_PATH='$ld_path'
export NGHTTP2INST=1
php tools/phase2/request-unary.php \
  --suite=request-unary \
  --implementation=php-grpc-lite \
  --autoload=vendor/autoload.php \
  --output='$output_dir/$name.json' \
  --duration='$duration_sec' \
  --max-calls='$max_calls' \
  --request-payload-sizes='$payload_bytes' \
  --warmup-calls=0 \
  --diagnostic-rpc \
  --curl-trace-output='$output_dir/$name-curl.log' \
  --curl-trace-calls='$max_calls' \
  $* \
  2>'$output_dir/$name-nghttp2.log'
"
}

run_case postfields
run_case readcb --upload-read-callback

cat <<EOF
Saved instrumented traces:
  $output_dir/postfields-curl.log
  $output_dir/postfields-nghttp2.log
  $output_dir/readcb-curl.log
  $output_dir/readcb-nghttp2.log
EOF
