#!/usr/bin/env bash
#
# OTEL benchmark entrypoint.
#
# Usage:
#   ./bench/run.sh throughput-unary
#   ./bench/run.sh rtt-unary
#   ./bench/run.sh rtt-unary-diagnostic
#   ./bench/run.sh throughput-streaming
#   ./bench/run.sh large-streaming
#   ./bench/run.sh streaming-diagnostic
#   ./bench/run.sh payload-unary
#   ./bench/run.sh payload-unary-diagnostic
#   ./bench/run.sh payload-unary-diagnostic-cached
#   ./bench/run.sh payload-unary-return-transfer-fast-path
#   ./bench/run.sh request-unary-diagnostic
#   ./bench/run.sh payload-streaming
#   ./bench/run.sh metadata-header
#   ./bench/run.sh metadata-header-diagnostic
#
set -euo pipefail

cd "$(dirname "$0")/.."

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
export NO_PROXY="${NO_PROXY:-test-server,spanner-emulator,toxiproxy,otelop,localhost,127.0.0.1}"
export no_proxy="${no_proxy:-$NO_PROXY}"

if [[ "$implementation" == "ext-grpc" ]]; then
    container_service="${BENCH_CONTAINER_SERVICE:-dev-ext-grpc}"
    autoload_path="${BENCH_AUTOLOAD:-vendor/autoload.php}"
fi

docker compose up -d otelop

run_benchmark_php() {
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
        OTEL_EXPORTER_OTLP_ENDPOINT \
        NO_PROXY \
        no_proxy
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
        run_benchmark_php "Benchmark unary throughput" tools/benchmark/throughput-unary.php
        ;;
    rtt-unary)
        docker compose up -d toxiproxy
        run_benchmark_php "Benchmark RTT unary" tools/benchmark/rtt-unary.php
        ;;
    rtt-unary-diagnostic)
        docker compose up -d toxiproxy
        run_benchmark_php "Benchmark RTT unary RPC diagnostic" tools/benchmark/rtt-unary.php --diagnostic-rpc
        ;;
    throughput-streaming)
        run_benchmark_php "Benchmark streaming throughput" tools/benchmark/throughput-streaming.php
        ;;
    large-streaming)
        run_benchmark_php "Benchmark large streaming" tools/benchmark/large-streaming.php
        ;;
    streaming-diagnostic)
        run_benchmark_php "Benchmark streaming RPC diagnostic" tools/benchmark/streaming-diagnostic.php
        ;;
    payload-unary)
        run_benchmark_php "Benchmark unary payload sweep" tools/benchmark/payload-unary.php
        ;;
    payload-unary-diagnostic)
        run_benchmark_php "Benchmark unary payload RPC diagnostic" tools/benchmark/payload-unary.php --diagnostic-rpc
        ;;
    payload-unary-diagnostic-cached)
        run_benchmark_php "Benchmark unary payload RPC diagnostic with cached server payload" tools/benchmark/payload-unary.php --diagnostic-rpc --server-cached-payload
        ;;
    payload-unary-return-transfer-fast-path)
        run_benchmark_php "Benchmark unary payload RPC diagnostic with return-transfer fast path" tools/benchmark/payload-unary.php --diagnostic-rpc --server-cached-payload --return-transfer-fast-path
        ;;
    request-unary-diagnostic)
        run_benchmark_php "Benchmark large request / small response RPC diagnostic" tools/benchmark/request-unary.php --diagnostic-rpc
        ;;
    payload-streaming)
        run_benchmark_php "Benchmark streaming payload sweep" tools/benchmark/payload-streaming.php
        ;;
    metadata-header)
        run_benchmark_php "Benchmark metadata/header sweep" tools/benchmark/metadata-header.php
        ;;
    metadata-header-diagnostic)
        run_benchmark_php "Benchmark metadata/header RPC diagnostic" tools/benchmark/metadata-header.php --diagnostic-rpc
        ;;
    *)
        cat >&2 <<USAGE
Unknown Benchmark suite: $suite

Usage: ./bench/run.sh [throughput-unary|rtt-unary|rtt-unary-diagnostic|throughput-streaming|large-streaming|streaming-diagnostic|payload-unary|payload-unary-diagnostic|payload-unary-diagnostic-cached|payload-unary-return-transfer-fast-path|request-unary-diagnostic|payload-streaming|metadata-header|metadata-header-diagnostic]
USAGE
        exit 2
        ;;
esac

echo
echo "OTEL summary: run_id=$BENCH_OTEL_RUN_ID"
docker compose run --rm -e BENCH_OTEL_RUN_ID="$BENCH_OTEL_RUN_ID" dev php \
    tools/benchmark/otelop-summary.php \
    --run-id="$BENCH_OTEL_RUN_ID" \
    --suite="$suite" \
    --limit="${BENCH_OTEL_SUMMARY_LIMIT:-100000}"
