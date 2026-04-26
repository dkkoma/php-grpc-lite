#!/usr/bin/env bash
#
# Run the full PHPBench suite against both the php-grpc-lite implementation
# and the official ext-grpc, with a Spanner emulator restart between each
# environment to keep the spanner numbers comparable. The emulator
# accumulates internal state across queries which causes 1.5x-2x run-to-run
# variance otherwise.
#
# Outputs both phpbench aggregate tables sequentially. For a side-by-side
# diff, copy them into the comparison-* doc by hand (variance is high enough
# that automated diffing is not particularly useful right now).
#
# Usage:  ./bench/compare.sh
#
set -euo pipefail

cd "$(dirname "$0")/.."

restart_spanner() {
    echo "[$(date +%H:%M:%S)] restart spanner-emulator..."
    docker compose restart spanner-emulator >/dev/null
    # Spanner emulator becomes ready in well under a second; 2s is generous.
    sleep 2
}

run_section() {
    local label="$1"
    shift
    echo
    echo "==========================================="
    echo "  RUN: $label"
    echo "==========================================="
    "$@"
}

restart_spanner
run_section "php-grpc-lite (us)" \
    docker compose run --rm dev vendor/bin/phpbench run --report=aggregate

restart_spanner
run_section "ext-grpc" \
    docker compose run --rm dev-ext-grpc \
        bash -c 'cd bench-comparison && vendor/bin/phpbench run --report=aggregate'

echo
echo "==========================================="
echo "  Done. Note: Spanner numbers vary by ±20-50% across runs; helloworld"
echo "  numbers (UnaryLatencyBench / UnaryDelayBench) are stable to ±2%."
echo "==========================================="
