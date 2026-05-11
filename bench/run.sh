#!/usr/bin/env bash
#
# OTEL benchmark entrypoint.
#
# Usage:
#   ./bench/run.sh throughput-unary
#   ./bench/run.sh rtt-unary
#   ./bench/run.sh throughput-streaming
#   ./bench/run.sh large-streaming
#   ./bench/run.sh payload-unary
#   ./bench/run.sh payload-streaming
#   ./bench/run.sh metadata-header
#   ./bench/run.sh spanner-dml-unary-shape
#   ./bench/run.sh small-select-streaming
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
    throughput-streaming)
        run_benchmark_php "Benchmark streaming throughput" tools/benchmark/throughput-streaming.php
        ;;
    large-streaming)
        run_benchmark_php "Benchmark large streaming" tools/benchmark/large-streaming.php
        ;;
    payload-unary)
        run_benchmark_php "Benchmark unary payload sweep" tools/benchmark/payload-unary.php
        ;;
    payload-streaming)
        run_benchmark_php "Benchmark streaming payload sweep" tools/benchmark/payload-streaming.php
        ;;
    metadata-header)
        run_benchmark_php "Benchmark metadata/header sweep" tools/benchmark/metadata-header.php
        ;;
    spanner-dml-unary-shape)
        run_benchmark_php "Benchmark Spanner DML unary shape" tools/benchmark/unary-shape.php
        ;;
    small-select-streaming)
        run_benchmark_php "Benchmark small SELECT streaming 1x100B" tools/benchmark/small-select-streaming.php --streams=1000 --message-count=1 --payload-bytes=100 --native-response-mode=stream
        run_benchmark_php "Benchmark small SELECT streaming 1x1KiB" tools/benchmark/small-select-streaming.php --streams=1000 --message-count=1 --payload-bytes=1024 --native-response-mode=stream
        run_benchmark_php "Benchmark small SELECT streaming 1x4KiB" tools/benchmark/small-select-streaming.php --streams=1000 --message-count=1 --payload-bytes=4096 --native-response-mode=stream
        run_benchmark_php "Benchmark small SELECT streaming 1x10KiB" tools/benchmark/small-select-streaming.php --streams=1000 --message-count=1 --payload-bytes=10240 --native-response-mode=stream
        ;;
    *)
        cat >&2 <<USAGE
Unknown Benchmark suite: $suite

Usage: ./bench/run.sh [throughput-unary|rtt-unary|throughput-streaming|large-streaming|payload-unary|payload-streaming|metadata-header|spanner-dml-unary-shape|small-select-streaming]
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
