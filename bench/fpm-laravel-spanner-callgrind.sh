#!/usr/bin/env bash
#
# Run the Laravel + colopl/laravel-spanner native path under callgrind.
#
# Usage:
#   ./bench/fpm-laravel-spanner-callgrind.sh [action] [iterations]
#
set -euo pipefail

cd "$(dirname "$0")/.."

action="${1:-select_1row_10col}"
iterations="${2:-10}"
run_id="${BENCH_RUN_ID:-$(date +%Y%m%d-%H%M%S)}"
log_dir="${BENCH_LOG_DIR:-var/bench-results/native-callgrind-$run_id}"
export COMPOSE_IGNORE_ORPHANS=True
export LARAVEL_SPANNER_EMULATOR_HOST="${LARAVEL_SPANNER_EMULATOR_HOST-spanner-emulator:9010}"
export LARAVEL_SPANNER_PROJECT_ID="${LARAVEL_SPANNER_PROJECT_ID:-test-project}"
export LARAVEL_SPANNER_INSTANCE_ID="${LARAVEL_SPANNER_INSTANCE_ID:-laravel-bench-instance}"
export LARAVEL_SPANNER_DATABASE_ID="${LARAVEL_SPANNER_DATABASE_ID:-laravel-bench-db}"
export LARAVEL_SPANNER_MIN_SESSIONS="${LARAVEL_SPANNER_MIN_SESSIONS:-16}"

mkdir -p "$log_dir"
exec > >(tee -a "$log_dir/runner.log") 2>&1

printf 'log_dir=%s\n' "$log_dir"
printf 'action=%s iterations=%s project=%s instance=%s database=%s emulator_host=%s min_sessions=%s\n' \
    "$action" \
    "$iterations" \
    "$LARAVEL_SPANNER_PROJECT_ID" \
    "$LARAVEL_SPANNER_INSTANCE_ID" \
    "$LARAVEL_SPANNER_DATABASE_ID" \
    "${LARAVEL_SPANNER_EMULATOR_HOST:-<cloud>}" \
    "$LARAVEL_SPANNER_MIN_SESSIONS"

docker compose run --rm fpm-lifecycle-profile sh -lc "
    mkdir -p /workspace/$log_dir
    php -d extension=/workspace/ext/grpc/modules/grpc.so -d opcache.enable_cli=1 /workspace/tools/benchmark/laravel-spanner-app/bin/startup-warmup.php
    valgrind \
        --tool=callgrind \
        --callgrind-out-file=/workspace/$log_dir/callgrind.out \
        php -d extension=/workspace/ext/grpc/modules/grpc.so -d opcache.enable_cli=1 \
        /workspace/tools/benchmark/laravel-spanner-app/bin/profile-action.php \
        '$action' \
        '$iterations'
"

docker compose run --rm fpm-lifecycle-profile sh -lc "
    callgrind_annotate \
        --threshold=0.5 \
        /workspace/$log_dir/callgrind.out \
        > /workspace/$log_dir/callgrind.annotate.txt
"

sed -n '1,180p' "$log_dir/callgrind.annotate.txt"
