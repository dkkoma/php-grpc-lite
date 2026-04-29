#!/usr/bin/env bash
#
# Phase 2 exploratory benchmark entrypoint.
#
# This runner is intentionally separated from bench/run.sh. Phase 2 suites are
# for deciding C-extension scope, not for Phase 1 regression baseline checks.
#
# Usage:
#   ./bench/phase2/run.sh contract-smoke
#   ./bench/phase2/run.sh cpu-memory-smoke
#   ./bench/phase2/run.sh throughput-unary
#   ./bench/phase2/run.sh rtt-unary
#   ./bench/phase2/run.sh rtt-unary-diagnostic
#   ./bench/phase2/run.sh throughput-streaming
#   ./bench/phase2/run.sh large-streaming
#   ./bench/phase2/run.sh payload-unary
#   ./bench/phase2/run.sh payload-unary-diagnostic
#   ./bench/phase2/run.sh payload-unary-diagnostic-cached
#   ./bench/phase2/run.sh payload-breakdown
#   ./bench/phase2/run.sh payload-streaming
#   ./bench/phase2/run.sh metadata-header
#
set -euo pipefail

cd "$(dirname "$0")/../.."

suite="${1:-contract-smoke}"
if [[ $# -gt 0 ]]; then
    shift
fi
extra_args=("$@")
timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results}"
implementation="${BENCH_IMPLEMENTATION:-php-grpc-lite}"
container_service="${BENCH_CONTAINER_SERVICE:-dev}"
autoload_path="${BENCH_AUTOLOAD:-vendor/autoload.php}"

if [[ "$implementation" == "ext-grpc" ]]; then
    container_service="${BENCH_CONTAINER_SERVICE:-dev-ext-grpc}"
    autoload_path="${BENCH_AUTOLOAD:-bench-comparison/vendor/autoload.php}"
fi

mkdir -p "$output_dir"

run_phase2_php() {
    local label="$1"
    local output_name="$2"
    shift 2

    local output_path="$output_dir/$output_name"

    echo
    echo "==========================================="
    echo "  RUN: $label"
    echo "  JSON: $output_path"
    echo "==========================================="

    docker compose run --rm "$container_service" php "$@" \
        --suite="$suite" \
        --implementation="$implementation" \
        --autoload="$autoload_path" \
        --output="$output_path" \
        "${extra_args[@]+"${extra_args[@]}"}"
}

case "$suite" in
    contract-smoke)
        run_phase2_php \
            "Phase 2 result contract smoke" \
            "phase2-$suite-$timestamp-$implementation.json" \
            tools/phase2/contract-smoke.php
        ;;
    cpu-memory-smoke)
        run_phase2_php \
            "Phase 2 CPU / memory sampling smoke" \
            "phase2-$suite-$timestamp-$implementation.json" \
            tools/phase2/cpu-memory-smoke.php
        ;;
    throughput-unary)
        run_phase2_php \
            "Phase 2 unary throughput" \
            "phase2-$suite-$timestamp-$implementation.json" \
            tools/phase2/throughput-unary.php
        ;;
    rtt-unary)
        docker compose up -d toxiproxy
        run_phase2_php \
            "Phase 2 RTT unary" \
            "phase2-$suite-$timestamp-$implementation.json" \
            tools/phase2/rtt-unary.php
        ;;
    rtt-unary-diagnostic)
        docker compose up -d toxiproxy
        run_phase2_php \
            "Phase 2 RTT unary RPC diagnostic" \
            "phase2-$suite-$timestamp-$implementation.json" \
            tools/phase2/rtt-unary.php \
            --diagnostic-rpc
        ;;
    throughput-streaming)
        run_phase2_php \
            "Phase 2 streaming throughput" \
            "phase2-$suite-$timestamp-$implementation.json" \
            tools/phase2/throughput-streaming.php
        ;;
    large-streaming)
        run_phase2_php \
            "Phase 2 large streaming" \
            "phase2-$suite-$timestamp-$implementation.json" \
            tools/phase2/large-streaming.php
        ;;
    payload-unary)
        run_phase2_php \
            "Phase 2 unary payload sweep" \
            "phase2-$suite-$timestamp-$implementation.json" \
            tools/phase2/payload-unary.php
        ;;
    payload-unary-diagnostic)
        run_phase2_php \
            "Phase 2 unary payload RPC diagnostic" \
            "phase2-$suite-$timestamp-$implementation.json" \
            tools/phase2/payload-unary.php \
            --diagnostic-rpc
        ;;
    payload-unary-diagnostic-cached)
        run_phase2_php \
            "Phase 2 unary payload RPC diagnostic with cached server payload" \
            "phase2-$suite-$timestamp-$implementation.json" \
            tools/phase2/payload-unary.php \
            --diagnostic-rpc \
            --server-cached-payload
        ;;
    payload-breakdown)
        run_phase2_php \
            "Phase 2 payload hot-path breakdown" \
            "phase2-$suite-$timestamp-$implementation.json" \
            tools/phase2/payload-breakdown.php
        ;;
    payload-streaming)
        run_phase2_php \
            "Phase 2 streaming payload sweep" \
            "phase2-$suite-$timestamp-$implementation.json" \
            tools/phase2/payload-streaming.php
        ;;
    metadata-header)
        run_phase2_php \
            "Phase 2 metadata/header sweep" \
            "phase2-$suite-$timestamp-$implementation.json" \
            tools/phase2/metadata-header.php
        ;;
    *)
        cat >&2 <<EOF
Unknown Phase 2 suite: $suite

Usage: ./bench/phase2/run.sh [contract-smoke|cpu-memory-smoke|throughput-unary|rtt-unary|rtt-unary-diagnostic|throughput-streaming|large-streaming|payload-unary|payload-unary-diagnostic|payload-unary-diagnostic-cached|payload-breakdown|payload-streaming|metadata-header]
EOF
        exit 2
        ;;
esac

echo
echo "Saved JSON: $output_dir/phase2-$suite-$timestamp-$implementation.json"
