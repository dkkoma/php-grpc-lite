#!/usr/bin/env bash
#
# Run request-unary diagnostics through the instrumented libcurl build.
#
# This runner intentionally covers only the upload path investigation. It emits
# curl debug traces with [CURLINST] records from lib/http2.c.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

output_dir="${BENCH_OUTPUT_DIR:-var/bench-results/instrumented-curl-$(date +%Y%m%d-%H%M%S)}"
payload_bytes="${BENCH_PAYLOAD_BYTES:-1048576}"
max_calls="${BENCH_MAX_CALLS:-3}"
duration_sec="${BENCH_DURATION:-1}"
libcurl_dir="var/instrumented-curl/lib"

if [[ ! -f "$libcurl_dir/libcurl.so.4" ]]; then
    cat >&2 <<EOF
Missing instrumented libcurl: $libcurl_dir/libcurl.so.4

Build it first:
  ./bench/phase2/build-instrumented-libcurl.sh
EOF
    exit 2
fi

mkdir -p "$output_dir"

run_case() {
    local name="$1"
    shift

    docker compose run --rm dev sh -lc "
set -euo pipefail
LD_LIBRARY_PATH=/workspace/var/instrumented-curl/lib php tools/phase2/request-unary.php \
  --suite=request-unary \
  --implementation=php-grpc-lite \
  --autoload=vendor/autoload.php \
  --output='$output_dir/$name.json' \
  --duration='$duration_sec' \
  --max-calls='$max_calls' \
  --request-payload-sizes='$payload_bytes' \
  --warmup-calls=0 \
  --diagnostic-rpc \
  --curl-trace-output='$output_dir/$name.log' \
  --curl-trace-calls='$max_calls' \
  $*
"
}

run_case postfields
run_case readcb --upload-read-callback

cat <<EOF
Saved instrumented libcurl traces:
  $output_dir/postfields.log
  $output_dir/readcb.log
EOF
