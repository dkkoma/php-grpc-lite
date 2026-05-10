#!/usr/bin/env bash
#
# Phase 2 OTEL benchmark entrypoint.
#
# Usage:
#   ./bench/phase2/run.sh throughput-unary
#   ./bench/phase2/run.sh rtt-unary
#   ./bench/phase2/run.sh rtt-unary-diagnostic
#   ./bench/phase2/run.sh throughput-streaming
#   ./bench/phase2/run.sh large-streaming
#   ./bench/phase2/run.sh streaming-diagnostic
#   ./bench/phase2/run.sh payload-unary
#   ./bench/phase2/run.sh payload-unary-diagnostic
#   ./bench/phase2/run.sh payload-unary-diagnostic-cached
#   ./bench/phase2/run.sh payload-unary-return-transfer-fast-path
#   ./bench/phase2/run.sh request-unary-diagnostic
#   ./bench/phase2/run.sh payload-streaming
#   ./bench/phase2/run.sh metadata-header
#   ./bench/phase2/run.sh metadata-header-diagnostic
#
set -euo pipefail

cd "$(dirname "$0")/../.."

suite="${1:-throughput-unary}"
if [[ $# -gt 0 ]]; then
    shift
fi
extra_args=("$@")
timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
implementation="${BENCH_IMPLEMENTATION:-php-grpc-lite}"
container_service="${BENCH_CONTAINER_SERVICE:-dev}"
autoload_path="${BENCH_AUTOLOAD:-vendor/autoload.php}"
export BENCH_OTEL_RUN_ID="${BENCH_OTEL_RUN_ID:-$timestamp}"
export BENCH_OTEL_EXPORTER="${BENCH_OTEL_EXPORTER:-otlp-http}"
export BENCH_OTEL_EXPORTER_OTLP_ENDPOINT="${BENCH_OTEL_EXPORTER_OTLP_ENDPOINT:-http://otelop:4318/v1/traces}"

if [[ "$implementation" == "ext-grpc" ]]; then
    container_service="${BENCH_CONTAINER_SERVICE:-dev-ext-grpc}"
    autoload_path="${BENCH_AUTOLOAD:-vendor/autoload.php}"
fi

docker compose up -d otelop

run_phase2_php() {
    local label="$1"
    shift

    local php_args=()
    if [[ "$implementation" == "php-grpc-lite" ]]; then
        php_args=(-d extension=/workspace/ext/grpc/modules/grpc.so)
    fi
    local docker_env=()
    local env_name
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

    echo
    echo "==========================================="
    echo "  RUN: $label"
    echo "  OTEL run id: $BENCH_OTEL_RUN_ID"
    echo "==========================================="

    docker compose run --rm "${docker_env[@]+"${docker_env[@]}"}" "$container_service" php ${php_args+"${php_args[@]}"} "$@" \
        --suite="$suite" \
        --implementation="$implementation" \
        --autoload="$autoload_path" \
        "${extra_args[@]+"${extra_args[@]}"}"
}

case "$suite" in
    throughput-unary)
        run_phase2_php "Phase 2 unary throughput" tools/phase2/throughput-unary.php
        ;;
    rtt-unary)
        docker compose up -d toxiproxy
        run_phase2_php "Phase 2 RTT unary" tools/phase2/rtt-unary.php
        ;;
    rtt-unary-diagnostic)
        docker compose up -d toxiproxy
        run_phase2_php "Phase 2 RTT unary RPC diagnostic" tools/phase2/rtt-unary.php --diagnostic-rpc
        ;;
    throughput-streaming)
        run_phase2_php "Phase 2 streaming throughput" tools/phase2/throughput-streaming.php
        ;;
    large-streaming)
        run_phase2_php "Phase 2 large streaming" tools/phase2/large-streaming.php
        ;;
    streaming-diagnostic)
        run_phase2_php "Phase 2 streaming RPC diagnostic" tools/phase2/streaming-diagnostic.php
        ;;
    payload-unary)
        run_phase2_php "Phase 2 unary payload sweep" tools/phase2/payload-unary.php
        ;;
    payload-unary-diagnostic)
        run_phase2_php "Phase 2 unary payload RPC diagnostic" tools/phase2/payload-unary.php --diagnostic-rpc
        ;;
    payload-unary-diagnostic-cached)
        run_phase2_php "Phase 2 unary payload RPC diagnostic with cached server payload" tools/phase2/payload-unary.php --diagnostic-rpc --server-cached-payload
        ;;
    payload-unary-return-transfer-fast-path)
        run_phase2_php "Phase 2 unary payload RPC diagnostic with return-transfer fast path" tools/phase2/payload-unary.php --diagnostic-rpc --server-cached-payload --return-transfer-fast-path
        ;;
    request-unary-diagnostic)
        run_phase2_php "Phase 2 large request / small response RPC diagnostic" tools/phase2/request-unary.php --diagnostic-rpc
        ;;
    payload-streaming)
        run_phase2_php "Phase 2 streaming payload sweep" tools/phase2/payload-streaming.php
        ;;
    metadata-header)
        run_phase2_php "Phase 2 metadata/header sweep" tools/phase2/metadata-header.php
        ;;
    metadata-header-diagnostic)
        run_phase2_php "Phase 2 metadata/header RPC diagnostic" tools/phase2/metadata-header.php --diagnostic-rpc
        ;;
    *)
        cat >&2 <<USAGE
Unknown Phase 2 suite: $suite

Usage: ./bench/phase2/run.sh [throughput-unary|rtt-unary|rtt-unary-diagnostic|throughput-streaming|large-streaming|streaming-diagnostic|payload-unary|payload-unary-diagnostic|payload-unary-diagnostic-cached|payload-unary-return-transfer-fast-path|request-unary-diagnostic|payload-streaming|metadata-header|metadata-header-diagnostic]
USAGE
        exit 2
        ;;
esac

echo
echo "OTEL summary: run_id=$BENCH_OTEL_RUN_ID"
docker compose run --rm -e BENCH_OTEL_RUN_ID="$BENCH_OTEL_RUN_ID" dev php \
    tools/phase2/otelop-summary.php \
    --run-id="$BENCH_OTEL_RUN_ID" \
    --suite="$suite" \
    --limit="${BENCH_OTEL_SUMMARY_LIMIT:-100000}"
