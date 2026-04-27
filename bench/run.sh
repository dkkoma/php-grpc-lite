#!/usr/bin/env bash
#
# Benchmark entrypoint for repeatable local runs. The regular comparison line
# remains php-grpc-lite vs official ext-grpc; grpc-php-rs stays in compare-rs.sh.
#
# Usage:
#   ./bench/run.sh lite
#   ./bench/run.sh ext
#   ./bench/run.sh compare
#   ./bench/run.sh cold
#   ./bench/run.sh stream
#   ./bench/run.sh hot-path
#
set -euo pipefail

cd "$(dirname "$0")/.."

suite="${1:-compare}"
timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results}"
mkdir -p "$output_dir"

run_logged() {
    local label="$1"
    local log_name="$2"
    shift 2

    local log_path="$output_dir/$suite-$timestamp-$log_name.log"

    echo
    echo "==========================================="
    echo "  RUN: $label"
    echo "  LOG: $log_path"
    echo "==========================================="
    "$@" | tee "$log_path"
}

run_lite() {
    local target="${1:-}"
    if [[ "$target" == "" ]]; then
        run_logged "php-grpc-lite full" "php-grpc-lite" \
            docker compose run --rm dev vendor/bin/phpbench run --report=aggregate
    else
        run_logged "php-grpc-lite $target" "php-grpc-lite" \
            docker compose run --rm dev vendor/bin/phpbench run "$target" --report=aggregate
    fi
}

run_ext() {
    local target="${1:-}"
    if [[ "$target" == "" ]]; then
        run_logged "official ext-grpc full" "ext-grpc" \
            docker compose run --rm dev-ext-grpc \
                bash -c 'cd bench-comparison && vendor/bin/phpbench run --report=aggregate'
    else
        run_logged "official ext-grpc $target" "ext-grpc" \
            docker compose run --rm dev-ext-grpc \
                bash -c "cd bench-comparison && vendor/bin/phpbench run ../$target --report=aggregate"
    fi
}

case "$suite" in
    lite)
        run_lite
        ;;
    ext)
        run_ext
        ;;
    compare)
        run_lite
        run_ext
        ;;
    cold)
        run_lite "bench/ColdUnaryBench.php"
        run_ext "bench/ColdUnaryBench.php"
        ;;
    stream)
        run_lite "bench/ServerStreamingBench.php"
        run_ext "bench/ServerStreamingBench.php"
        ;;
    hot-path)
        run_logged "php-grpc-lite local hot path split" "hot-path" \
            docker compose run --rm dev php tools/bench-hot-path.php
        ;;
    *)
        cat >&2 <<EOF
Unknown benchmark suite: $suite

Usage: ./bench/run.sh [lite|ext|compare|cold|stream|hot-path]
EOF
        exit 2
        ;;
esac

echo
echo "Saved logs: $output_dir/$suite-$timestamp-*.log"
