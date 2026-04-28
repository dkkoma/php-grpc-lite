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

    docker compose run --rm dev php "$@" \
        --suite="$suite" \
        --implementation="$implementation" \
        --output="$output_path" \
        "${extra_args[@]}"
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
    *)
        cat >&2 <<EOF
Unknown Phase 2 suite: $suite

Usage: ./bench/phase2/run.sh [contract-smoke|cpu-memory-smoke|throughput-unary|rtt-unary]
EOF
        exit 2
        ;;
esac

echo
echo "Saved JSON: $output_dir/phase2-$suite-$timestamp-$implementation.json"
