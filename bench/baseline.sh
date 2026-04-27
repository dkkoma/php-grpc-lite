#!/usr/bin/env bash
#
# Operate the php-grpc-lite regression baseline.
#
# Usage:
#   ./bench/baseline.sh check
#   ./bench/baseline.sh update
#
set -euo pipefail

cd "$(dirname "$0")/.."

action="${1:-check}"
timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results}"
baseline_path="${BENCH_BASELINE:-bench/baselines/regression.json}"
implementation="php-grpc-lite"

mkdir -p "$output_dir"

run_suite() {
    local suite="$1"
    local bench_path="$2"
    local tag="$3"
    local compare="$4"
    local log_path="$output_dir/$suite-$tag-$implementation.log"
    local json_path="${log_path%.log}.json"
    local tsv_path="${log_path%.log}.tsv"
    local cmd=(
        docker compose run --rm dev bench/phpbench-with-artifacts.sh
        --workdir=.
        --log="$log_path"
        --json="$json_path"
        --tsv="$tsv_path"
    )

    if [[ "$compare" == "yes" ]]; then
        cmd+=(
            --baseline="$baseline_path"
            --suite="$suite"
            --implementation="$implementation"
        )
    fi

    echo
    echo "==========================================="
    echo "  RUN: baseline $action $suite"
    echo "  LOG: $log_path"
    echo "==========================================="

    "${cmd[@]}" -- vendor/bin/phpbench run "$bench_path" --report=aggregate

    if [[ "$action" == "update" ]]; then
        docker compose run --rm dev php tools/update-benchmark-baseline.php \
            --baseline="$baseline_path" \
            --current="$json_path" \
            --suite="$suite" \
            --implementation="$implementation" \
            --source="$json_path"
    fi
}

case "$action" in
    check)
        compare="yes"
        ;;
    update)
        compare="no"
        ;;
    *)
        cat >&2 <<EOF
Unknown baseline action: $action

Usage: ./bench/baseline.sh [check|update]
EOF
        exit 2
        ;;
esac

run_suite cold "bench/ColdUnaryBench.php" "$action-$timestamp" "$compare"
run_suite warm "bench/UnaryLatencyBench.php" "$action-$timestamp" "$compare"
run_suite stream-smoke "bench/ServerStreamingCount1000Bench.php" "$action-$timestamp" "$compare"

echo
echo "Baseline $action completed: $baseline_path"
