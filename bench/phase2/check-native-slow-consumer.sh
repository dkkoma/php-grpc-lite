#!/usr/bin/env bash
#
# Check actual native server-streaming surface under slow consumer behavior.
# The current MVP native wrapper drains a full batch before yielding, so this
# runner records that limitation instead of treating it as a production result.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results}"
sleep_us="${SLEEP_US:-1000}"
streams="${STREAMS:-10}"
message_count="${MESSAGE_COUNT:-100}"
payload_bytes="${PAYLOAD_BYTES:-100}"
mkdir -p "$output_dir"

output="$output_dir/phase2-native-slow-consumer-$timestamp.json"

docker compose run --rm dev sh -lc \
    "php -d extension=/workspace/poc/nghttp2-client-ext/modules/nghttp2_poc.so tools/phase2/slow-consumer-surface.php --output='$output' --streams=$streams --message-count=$message_count --payload-bytes=$payload_bytes --sleep-us=$sleep_us --transport=native --native-response-mode=direct"

echo "JSON: $output"
