#!/usr/bin/env bash
#
# Compare slow-consumer server-streaming behavior between php-grpc-lite's
# nghttp2 extension surface and official ext-grpc. Results are printed only;
# benchmark result persistence is handled through OTEL benchmark runners.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

sleep_us="${SLEEP_US:-1000}"
streams="${STREAMS:-10}"
message_count="${MESSAGE_COUNT:-100}"
payload_bytes="${PAYLOAD_BYTES:-1024}"

echo "== php-grpc-lite slow consumer =="
docker compose run --rm dev sh -lc     "php -d extension=/workspace/ext/grpc/modules/grpc.so tools/benchmark/slow-consumer-surface.php --implementation=php-grpc-lite --autoload=vendor/autoload.php --streams=$streams --message-count=$message_count --payload-bytes=$payload_bytes --sleep-us=$sleep_us --native-response-mode=stream"

echo "== ext-grpc slow consumer =="
docker compose run --rm dev-ext-grpc php tools/benchmark/slow-consumer-surface.php     --implementation=ext-grpc     --autoload=vendor/autoload.php     --streams="$streams"     --message-count="$message_count"     --payload-bytes="$payload_bytes"     --sleep-us="$sleep_us"
