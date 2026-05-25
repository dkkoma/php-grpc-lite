#!/usr/bin/env bash
#
# Run the same benchmark suite against php-grpc-lite and official ext-grpc.
#
# Usage:
#   ./bench/compare.sh throughput-unary
#
set -euo pipefail

cd "$(dirname "$0")/.."

suite="${1:-throughput-unary}"
if [[ $# -gt 0 ]]; then
    shift
fi

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
export BENCH_TAG="$timestamp"
export BENCH_OTEL_RUN_ID="${BENCH_OTEL_RUN_ID:-$timestamp}"

echo
echo "==========================================="
echo "  BENCHMARK COMPARE: $suite"
echo "  TAG: $BENCH_TAG"
echo "  OTEL run id: $BENCH_OTEL_RUN_ID"
echo "==========================================="

BENCH_IMPLEMENTATION=php-grpc-lite ./bench/run.sh "$suite" "$@"
BENCH_IMPLEMENTATION=ext-grpc ./bench/run.sh "$suite" "$@"

echo
echo "Combined OTEL summary: run_id=$BENCH_OTEL_RUN_ID"
docker compose run --rm -e BENCH_OTEL_RUN_ID="$BENCH_OTEL_RUN_ID" dev php -d memory_limit=-1 \
    tools/benchmark/otelop-summary.php \
    --run-id="$BENCH_OTEL_RUN_ID" \
    --suite="$suite" \
    --limit="${BENCH_OTEL_SUMMARY_LIMIT:-100000}"
