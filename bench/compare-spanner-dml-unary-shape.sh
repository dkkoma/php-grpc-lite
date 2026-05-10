#!/usr/bin/env bash
#
# Compare unary request/response shapes observed from Spanner DML flow.
# Results are exported as OTEL spans and summarized from otelop.
#
set -euo pipefail

cd "$(dirname "$0")/.."

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
duration="${DURATION:-1}"
warmup_calls="${WARMUP_CALLS:-3}"
max_calls="${MAX_CALLS:-0}"
include_franken="${INCLUDE_FRANKEN:-0}"
export BENCH_OTEL_RUN_ID="${BENCH_OTEL_RUN_ID:-$timestamp}"
export BENCH_OTEL_EXPORTER="${BENCH_OTEL_EXPORTER:-otlp-http}"
export BENCH_OTEL_EXPORTER_OTLP_ENDPOINT="${BENCH_OTEL_EXPORTER_OTLP_ENDPOINT:-http://otelop:4318/v1/traces}"
export NO_PROXY="${NO_PROXY:-test-server,spanner-emulator,toxiproxy,otelop,localhost,127.0.0.1}"
export no_proxy="${no_proxy:-$NO_PROXY}"

docker compose up -d otelop

docker_env=()
for env_name in \
    BENCH_OTEL_EXPORTER \
    BENCH_OTEL_EXPORTER_OTLP_ENDPOINT \
    BENCH_OTEL_RUN_ID \
    OTEL_EXPORTER_OTLP_TRACES_ENDPOINT \
    OTEL_EXPORTER_OTLP_ENDPOINT \
    NO_PROXY \
    no_proxy
do
    if [[ -n "${!env_name:-}" ]]; then
        docker_env+=(-e "$env_name=${!env_name}")
    fi
done

run_args=(
    --suite=spanner-dml-unary-shape
    --autoload=vendor/autoload.php
    --duration="$duration"
    --warmup-calls="$warmup_calls"
    --max-calls="$max_calls"
)

echo "== php-grpc-lite native =="
docker compose run --rm "${docker_env[@]+"${docker_env[@]}"}" dev sh -lc \
    "php -d extension=/workspace/ext/grpc/modules/grpc.so tools/benchmark/unary-shape.php --implementation=php-grpc-lite ${run_args[*]}"

if [[ "$include_franken" == "1" ]]; then
    echo "== php-grpc-lite franken-go =="
    docker compose run --rm "${docker_env[@]+"${docker_env[@]}"}" dev-franken-grpc-go tools/frankenphp-grpc-lite-run.sh tools/benchmark/unary-shape.php \
        --implementation=php-grpc-lite \
        --transport=franken-go \
        "${run_args[@]}"
fi

echo "== ext-grpc =="
docker compose run --rm "${docker_env[@]+"${docker_env[@]}"}" dev-ext-grpc php tools/benchmark/unary-shape.php \
    --implementation=ext-grpc \
    "${run_args[@]}"

echo
echo "OTEL summary: run_id=$BENCH_OTEL_RUN_ID"
docker compose run --rm -e BENCH_OTEL_RUN_ID="$BENCH_OTEL_RUN_ID" dev php \
    tools/benchmark/otelop-summary.php \
    --run-id="$BENCH_OTEL_RUN_ID" \
    --suite=spanner-dml-unary-shape \
    --limit="${BENCH_OTEL_SUMMARY_LIMIT:-100000}"
