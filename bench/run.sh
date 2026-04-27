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
#   ./bench/run.sh warm
#   ./bench/run.sh stream
#   ./bench/run.sh stream-smoke
#   ./bench/run.sh hot-path
#
set -euo pipefail

cd "$(dirname "$0")/.."

suite="${1:-compare}"
timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results}"
mkdir -p "$output_dir"
last_log_path=""
last_json_path=""

run_logged() {
    local label="$1"
    local log_name="$2"
    shift 2

    local log_path="$output_dir/$suite-$timestamp-$log_name.log"
    last_log_path="$log_path"

    echo
    echo "==========================================="
    echo "  RUN: $label"
    echo "  LOG: $log_path"
    echo "==========================================="
    "$@" | tee "$log_path"
}

parse_phpbench_log() {
    local log_path="$1"
    local json_path="${log_path%.log}.json"
    local tsv_path="${log_path%.log}.tsv"
    last_json_path="$json_path"

    for attempt in 1 2 3; do
        local stderr_target="/dev/null"
        if [[ "$attempt" == "3" ]]; then
            stderr_target="/dev/stderr"
        fi
        if docker compose run --rm dev php tools/parse-phpbench-aggregate.php \
            --format=json \
            --output="$json_path" \
            "$log_path" >/dev/null 2>"$stderr_target"; then
            break
        fi
        if [[ "$attempt" == "3" ]]; then
            return 1
        fi
        sleep 0.2
    done

    docker compose run --rm dev php tools/parse-phpbench-aggregate.php \
        --format=tsv \
        --output="$tsv_path" \
        "$log_path" >/dev/null

    echo "  JSON: $json_path"
    echo "  TSV: $tsv_path"
}

compare_baseline() {
    local implementation="$1"

    if [[ "${BENCH_BASELINE:-}" == "" ]]; then
        return
    fi

    docker compose run --rm dev php tools/compare-benchmark-baseline.php \
        --baseline="$BENCH_BASELINE" \
        --current="$last_json_path" \
        --suite="$suite" \
        --implementation="$implementation"
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
    parse_phpbench_log "$last_log_path"
    compare_baseline "php-grpc-lite"
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
    parse_phpbench_log "$last_log_path"
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
    warm)
        run_lite "bench/UnaryLatencyBench.php"
        run_ext "bench/UnaryLatencyBench.php"
        ;;
    stream)
        run_lite "bench/ServerStreamingBench.php"
        run_ext "bench/ServerStreamingBench.php"
        ;;
    stream-smoke)
        run_lite "bench/ServerStreamingCount1000Bench.php"
        run_ext "bench/ServerStreamingCount1000Bench.php"
        ;;
    hot-path)
        run_logged "php-grpc-lite local hot path split" "hot-path" \
            docker compose run --rm dev php tools/bench-hot-path.php
        ;;
    *)
        cat >&2 <<EOF
Unknown benchmark suite: $suite

Usage: ./bench/run.sh [lite|ext|compare|cold|warm|stream|stream-smoke|hot-path]
EOF
        exit 2
        ;;
esac

echo
echo "Saved logs: $output_dir/$suite-$timestamp-*.log"
