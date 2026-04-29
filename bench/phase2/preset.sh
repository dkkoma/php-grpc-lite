#!/usr/bin/env bash
#
# Run Phase 2 benchmark presets.
#
# Presets intentionally keep orchestration in shell and suite implementation in
# tools/phase2/*.php.  Use this when the question is "which comparable data set
# should we collect?", not when iterating on one runner.
#
# Usage:
#   ./bench/phase2/preset.sh smoke
#   ./bench/phase2/preset.sh compare
#   ./bench/phase2/preset.sh decision
#
set -euo pipefail

cd "$(dirname "$0")/../.."

preset="${1:-smoke}"
timestamp="${BENCH_TAG:-phase2-$(date +%Y%m%d-%H%M%S)}"
export BENCH_TAG="$timestamp"

run_single() {
    local suite="$1"
    shift

    ./bench/phase2/run.sh "$suite" "$@"
}

run_compare() {
    local suite="$1"
    shift

    ./bench/phase2/compare.sh "$suite" "$@"
}

validate_json() {
    docker compose run --rm \
        -e BENCH_TAG="$BENCH_TAG" \
        -e BENCH_OUTPUT_DIR="${BENCH_OUTPUT_DIR:-var/bench-results}" \
        dev php -r '
        $class = "PhpGrpcLite\\Tools\\Phase2\\ResultContract";
        require "tools/phase2/ResultContract.php";
        $pattern = sprintf(
            "%s/phase2-*-%s-*.json",
            getenv("BENCH_OUTPUT_DIR") ?: "var/bench-results",
            getenv("BENCH_TAG")
        );
        $paths = glob($pattern);
        if ($paths === false || $paths === []) {
            fwrite(STDERR, "No Phase 2 JSON matched: {$pattern}\n");
            exit(1);
        }
        foreach ($paths as $path) {
            $document = json_decode((string) file_get_contents($path), true);
            $class::validate($document);
            echo "validated {$path}\n";
        }
    '
}

echo
echo "==========================================="
echo "  PHASE2 PRESET: $preset"
echo "  TAG: $BENCH_TAG"
echo "==========================================="

case "$preset" in
    smoke)
        run_single contract-smoke
        run_compare throughput-unary --duration=0.2 --warmup-calls=1
        run_compare throughput-streaming --duration=0.2 --message-count=100 --payload-bytes=100 --warmup-streams=1
        run_compare metadata-header --calls=2
        run_single payload-breakdown --payload-sizes=100,102400 --revs=100
        run_single payload-unary-diagnostic --duration=0.1 --payload-sizes=102400 --warmup-calls=1
        run_single payload-unary-diagnostic-cached --duration=0.1 --payload-sizes=102400 --warmup-calls=1
        run_single rtt-unary-diagnostic --calls=3 --warmup-calls=1
        ;;
    compare)
        run_compare throughput-unary --duration=1 --warmup-calls=3
        run_compare payload-unary --duration=0.5 --payload-sizes=0,100,1024,10240,102400 --warmup-calls=2
        run_single payload-breakdown --payload-sizes=0,100,1024,10240,102400
        run_single payload-unary-diagnostic --duration=0.5 --payload-sizes=102400 --warmup-calls=2
        run_single payload-unary-diagnostic-cached --duration=0.5 --payload-sizes=102400 --warmup-calls=2
        run_compare rtt-unary --calls=10 --warmup-calls=2
        run_single rtt-unary-diagnostic --calls=10 --warmup-calls=2
        run_compare throughput-streaming --duration=0.5 --message-count=100 --payload-bytes=100 --warmup-streams=1
        run_compare large-streaming --message-counts=1000,10000 --payload-bytes=100
        run_compare payload-streaming --streams=5 --message-count=100 --payload-sizes=0,100,1024,10240
        run_compare metadata-header --calls=10
        ;;
    decision)
        run_compare throughput-unary --duration=5 --warmup-calls=20
        run_compare payload-unary --duration=3 --payload-sizes=0,100,1024,10240,102400 --warmup-calls=10
        run_single payload-breakdown --payload-sizes=0,100,1024,10240,102400
        run_single payload-unary-diagnostic --duration=3 --payload-sizes=102400 --warmup-calls=10
        run_single payload-unary-diagnostic-cached --duration=3 --payload-sizes=102400 --warmup-calls=10
        run_compare rtt-unary --calls=50 --warmup-calls=10
        run_single rtt-unary-diagnostic --calls=50 --warmup-calls=10
        run_compare throughput-streaming --duration=5 --message-count=1000 --payload-bytes=100 --warmup-streams=3
        run_compare large-streaming --message-counts=10000,100000 --payload-bytes=100
        run_compare payload-streaming --streams=30 --message-count=100 --payload-sizes=0,100,1024,10240
        run_compare metadata-header --calls=100
        ;;
    *)
        cat >&2 <<EOF
Unknown Phase 2 preset: $preset

Usage: ./bench/phase2/preset.sh [smoke|compare|decision]

  smoke     Fast contract and comparable sanity checks.
  compare   Short exploratory php-grpc-lite vs ext-grpc comparison.
  decision  Longer comparison for optimization decisions.
EOF
        exit 2
        ;;
esac

validate_json

echo
echo "Phase 2 preset completed."
