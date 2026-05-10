#!/usr/bin/env bash
#
# Compare small server-streaming response shapes that approximate small SELECT
# results. Results are exported as OTEL spans and summarized from otelop.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
native_response_mode="${NATIVE_RESPONSE_MODE:-stream}"
include_franken="${INCLUDE_FRANKEN:-0}"
warmup_streams="${WARMUP_STREAMS:-3}"
export BENCH_OTEL_RUN_ID="${BENCH_OTEL_RUN_ID:-$timestamp}"
export BENCH_OTEL_EXPORTER="${BENCH_OTEL_EXPORTER:-otlp-http}"
export BENCH_OTEL_EXPORTER_OTLP_ENDPOINT="${BENCH_OTEL_EXPORTER_OTLP_ENDPOINT:-http://otelop:4318/v1/traces}"

docker compose up -d otelop

docker_env=()
for env_name in \
    BENCH_OTEL_EXPORTER \
    BENCH_OTEL_EXPORTER_OTLP_ENDPOINT \
    BENCH_OTEL_RUN_ID \
    OTEL_EXPORTER_OTLP_TRACES_ENDPOINT \
    OTEL_EXPORTER_OTLP_ENDPOINT
do
    if [[ -n "${!env_name:-}" ]]; then
        docker_env+=(-e "$env_name=${!env_name}")
    fi
done

declare -a cases=(
    "1x100b 1000 1 100"
    "1x1k 1000 1 1024"
    "1x4k 1000 1 4096"
    "1x10k 1000 1 10240"
)

for case_spec in "${cases[@]}"; do
    read -r case_name streams message_count payload_bytes <<< "$case_spec"
    common_args=(
        --suite=small-select-streaming
        --autoload=vendor/autoload.php
        --streams="$streams"
        --warmup-streams="$warmup_streams"
        --message-count="$message_count"
        --payload-bytes="$payload_bytes"
        --native-response-mode="$native_response_mode"
    )

    echo
    echo "== $case_name: php-grpc-lite native =="
    docker compose run --rm "${docker_env[@]+"${docker_env[@]}"}" dev sh -lc \
        "php -d extension=/workspace/ext/grpc/modules/grpc.so tools/phase2/streaming-diagnostic.php --implementation=php-grpc-lite ${common_args[*]}"

    if [[ "$include_franken" == "1" ]]; then
        echo "== $case_name: php-grpc-lite franken-go =="
        docker compose run --rm "${docker_env[@]+"${docker_env[@]}"}" dev-franken-grpc-go tools/frankenphp-grpc-lite-run.sh tools/phase2/streaming-diagnostic.php \
            --implementation=php-grpc-lite \
            --transport=franken-go \
            "${common_args[@]}"
    fi

    echo "== $case_name: ext-grpc =="
    docker compose run --rm "${docker_env[@]+"${docker_env[@]}"}" dev-ext-grpc php tools/phase2/streaming-diagnostic.php \
        --implementation=ext-grpc \
        "${common_args[@]}"
done

echo
echo "OTEL summary: run_id=$BENCH_OTEL_RUN_ID"
docker compose run --rm -e BENCH_OTEL_RUN_ID="$BENCH_OTEL_RUN_ID" dev php \
    tools/phase2/otelop-summary.php \
    --run-id="$BENCH_OTEL_RUN_ID" \
    --suite=small-select-streaming \
    --limit="${BENCH_OTEL_SUMMARY_LIMIT:-100000}"
