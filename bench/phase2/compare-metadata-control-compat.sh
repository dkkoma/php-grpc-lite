#!/usr/bin/env bash
#
# Observe reserved/fixed header and metadata validation behavior across
# grpc-lite and official ext-grpc.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"

echo
echo "==========================================="
echo "  PHASE2 METADATA CONTROL COMPAT"
echo "  TAG: $timestamp"
echo "==========================================="

BENCH_TAG="$timestamp-grpc-lite" BENCH_IMPLEMENTATION=php-grpc-lite ./bench/phase2/run.sh metadata-control-compat
BENCH_TAG="$timestamp-ext" BENCH_IMPLEMENTATION=ext-grpc ./bench/phase2/run.sh metadata-control-compat

echo
echo "Saved JSON:"
echo "  ${BENCH_OUTPUT_DIR:-var/bench-results}/phase2-metadata-control-compat-$timestamp-grpc-lite-php-grpc-lite.json"
echo "  ${BENCH_OUTPUT_DIR:-var/bench-results}/phase2-metadata-control-compat-$timestamp-ext-ext-grpc.json"
