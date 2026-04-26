#!/usr/bin/env bash
#
# Run the full PHPBench suite against both the php-grpc-lite implementation
# and the official ext-grpc. All benchmarks target the local Go test-server,
# which gives stable ±2-3% numbers — no spanner-emulator restart dance needed.
#
# Usage:  ./bench/compare.sh
#
set -euo pipefail

cd "$(dirname "$0")/.."

run_section() {
    local label="$1"
    shift
    echo
    echo "==========================================="
    echo "  RUN: $label"
    echo "==========================================="
    "$@"
}

run_section "php-grpc-lite (us)" \
    docker compose run --rm dev vendor/bin/phpbench run --report=aggregate

run_section "ext-grpc" \
    docker compose run --rm dev-ext-grpc \
        bash -c 'cd bench-comparison && vendor/bin/phpbench run --report=aggregate'
