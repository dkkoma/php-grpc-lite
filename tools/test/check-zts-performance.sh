#!/usr/bin/env bash
#
# Compare representative php-grpc-lite benchmarks between NTS and ZTS PHP.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

tag="${BENCH_TAG:-zts-compare-$(date +%Y%m%d-%H%M%S)}"
suites="${ZTS_PERF_SUITES:-spanner-shape metadata-header}"
bench_args=()
if [[ -n "${ZTS_PERF_ARGS:-}" ]]; then
    # shellcheck disable=SC2206
    bench_args=(${ZTS_PERF_ARGS})
fi

build_extension() {
    local service="$1"
    local label="$2"

    echo "== build extension for $label =="
    docker compose run --build --rm "$service" bash -lc '
        set -euo pipefail
        cd /workspace/ext/grpc
        make clean >/tmp/grpc-zts-perf-clean.log 2>&1 || true
        rm -rf .libs modules *.lo *.o *.dep
        phpize >/tmp/grpc-zts-perf-phpize.log
        ./configure --enable-grpc >/tmp/grpc-zts-perf-configure.log
        make -j$(nproc) >/tmp/grpc-zts-perf-make.log
        cd /workspace
        php -d extension=/workspace/ext/grpc/modules/grpc.so -r '\''exit(extension_loaded("grpc") ? 0 : 1);'\''
    '
}

run_suite_set() {
    local service="$1"
    local label="$2"

    build_extension "$service" "$label"

    local suite
    for suite in $suites; do
        echo "== benchmark $label: $suite =="
        BENCH_TAG="$tag-$label-$suite" \
        BENCH_CONTAINER_SERVICE="$service" \
        BENCH_IMPLEMENTATION=php-grpc-lite \
        BENCH_IMPLEMENTATION_LABEL="php-grpc-lite-$label" \
            ./bench/run.sh "$suite" "${bench_args[@]+"${bench_args[@]}"}"
    done
}

run_suite_set dev nts
run_suite_set dev-zts zts

cat <<SUMMARY

ZTS performance comparison completed.

Run id prefix: $tag
Suites: $suites

Use tools/benchmark/otelop-summary.php or the otelop UI to compare matching
NTS/ZTS run ids:
  $tag-nts-<suite>
  $tag-zts-<suite>
SUMMARY
