#!/usr/bin/env bash
#
# Optional one-shot comparison against BSN4/grpc-php-rs. The regular
# comparison remains ./bench/compare.sh (php-grpc-lite vs official ext-grpc).
#
# Usage: ./bench/compare-rs.sh
#
set -euo pipefail

cd "$(dirname "$0")/.."

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results}"
mkdir -p "$output_dir"

run_section() {
    local label="$1"
    local log_name="$2"
    shift 2

    local log_path="$output_dir/compare-rs-$timestamp-$log_name.log"

    echo
    echo "==========================================="
    echo "  RUN: $label"
    echo "  LOG: $log_path"
    echo "==========================================="
    "$@" | tee "$log_path"
}

run_section "php-grpc-lite (us)" "php-grpc-lite" \
    docker compose run --rm dev vendor/bin/phpbench run --report=aggregate

run_section "official ext-grpc" "ext-grpc" \
    docker compose run --rm dev-ext-grpc \
        bash -c 'cd bench-comparison && vendor/bin/phpbench run --report=aggregate'

run_section "grpc-php-rs" "grpc-php-rs" \
    docker compose run --rm dev-grpc-rs \
        bash -c 'cd bench-comparison && vendor/bin/phpbench run --report=aggregate'

echo
echo "Saved logs: $output_dir/compare-rs-$timestamp-*.log"
