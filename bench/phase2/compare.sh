#!/usr/bin/env bash
#
# Run the same Phase 2 suite against php-grpc-lite and official ext-grpc.
#
# Usage:
#   ./bench/phase2/compare.sh throughput-unary
#
set -euo pipefail

cd "$(dirname "$0")/../.."

suite="${1:-throughput-unary}"
if [[ $# -gt 0 ]]; then
    shift
fi

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
export BENCH_TAG="$timestamp"

echo
echo "==========================================="
echo "  PHASE2 COMPARE: $suite"
echo "  TAG: $BENCH_TAG"
echo "==========================================="

BENCH_IMPLEMENTATION=php-grpc-lite ./bench/phase2/run.sh "$suite" "$@"
BENCH_IMPLEMENTATION=ext-grpc ./bench/phase2/run.sh "$suite" "$@"

echo
echo "Saved JSON:"
echo "  ${BENCH_OUTPUT_DIR:-var/bench-results}/phase2-$suite-$BENCH_TAG-php-grpc-lite.json"
echo "  ${BENCH_OUTPUT_DIR:-var/bench-results}/phase2-$suite-$BENCH_TAG-ext-grpc.json"
