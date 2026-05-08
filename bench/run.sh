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
#   ./bench/run.sh stream-slow
#   ./bench/run.sh metadata
#   ./bench/run.sh tls
#   ./bench/run.sh hot-path
#
set -euo pipefail

cd "$(dirname "$0")/.."

suite="${1:-compare}"
timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results}"
mkdir -p "$output_dir"
last_log_path=""

run_hot_path() {
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

run_lite() {
    local bench_path="${1:-}"
    local log_path="$output_dir/$suite-$timestamp-php-grpc-lite.log"
    local cmd=(
        docker compose run --rm dev bench/phpbench-with-artifacts.sh
        --workdir=.
        --log="$log_path"
        --json="${log_path%.log}.json"
        --tsv="${log_path%.log}.tsv"
    )

    if [[ "${BENCH_BASELINE:-}" != "" ]]; then
        cmd+=(
            --baseline="$BENCH_BASELINE"
            --suite="$suite"
            --implementation=php-grpc-lite
        )
    fi

    echo
    echo "==========================================="
    echo "  RUN: php-grpc-lite ${bench_path:-full}"
    echo "  LOG: $log_path"
    echo "==========================================="

    if [[ "$bench_path" == "" ]]; then
        "${cmd[@]}" -- php -d extension=/workspace/ext/grpc/modules/grpc.so vendor/bin/phpbench run --php-config='{"extension":"/workspace/ext/grpc/modules/grpc.so"}' --report=aggregate
    else
        "${cmd[@]}" -- php -d extension=/workspace/ext/grpc/modules/grpc.so vendor/bin/phpbench run "$bench_path" --php-config='{"extension":"/workspace/ext/grpc/modules/grpc.so"}' --report=aggregate
    fi
}

run_ext() {
    local bench_path="${1:-}"
    local log_path="$output_dir/$suite-$timestamp-ext-grpc.log"

    echo
    echo "==========================================="
    echo "  RUN: official ext-grpc ${bench_path:-full}"
    echo "  LOG: $log_path"
    echo "==========================================="

    if [[ "$bench_path" == "" ]]; then
        docker compose run --rm dev-ext-grpc bench/phpbench-with-artifacts.sh \
            --workdir=. \
            --log="$log_path" \
            --json="${log_path%.log}.json" \
            --tsv="${log_path%.log}.tsv" \
            -- vendor/bin/phpbench run --report=aggregate
    else
        docker compose run --rm dev-ext-grpc bench/phpbench-with-artifacts.sh \
            --workdir=. \
            --log="$log_path" \
            --json="${log_path%.log}.json" \
            --tsv="${log_path%.log}.tsv" \
            -- vendor/bin/phpbench run "$bench_path" --report=aggregate
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
    stream-slow)
        run_lite "bench/ServerStreamingSlowConsumerBench.php"
        run_ext "bench/ServerStreamingSlowConsumerBench.php"
        ;;
    metadata)
        run_lite "bench/MetadataVolumeBench.php"
        run_ext "bench/MetadataVolumeBench.php"
        ;;
    tls)
        run_lite "bench/TlsUnaryBench.php"
        run_ext "bench/TlsUnaryBench.php"
        ;;
    hot-path)
        run_hot_path "php-grpc-lite local hot path split" "hot-path" \
            docker compose run --rm dev php tools/bench-hot-path.php
        ;;
    *)
        cat >&2 <<EOF
Unknown benchmark suite: $suite

Usage: ./bench/run.sh [lite|ext|compare|cold|warm|stream|stream-smoke|stream-slow|metadata|tls|hot-path]
EOF
        exit 2
        ;;
esac

echo
echo "Saved logs: $output_dir/$suite-$timestamp-*.log"
