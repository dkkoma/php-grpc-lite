#!/usr/bin/env bash
#
# Measure php-fpm worker CPU for Laravel + colopl/laravel-spanner request lifecycle.
#
# Usage:
#   ./bench/fpm-laravel-spanner-cpu-compare.sh [requests]
#
set -euo pipefail

cd "$(dirname "$0")/.."

requests="${1:-100}"
actions=(transaction_select2_update1_insert1)
app_dir="tools/benchmark/laravel-spanner-app"
run_id="${BENCH_RUN_ID:-$(date +%Y%m%d-%H%M%S)}"
log_dir="${BENCH_LOG_DIR:-var/bench-results/fpm-laravel-spanner-cpu-$run_id}"
export COMPOSE_IGNORE_ORPHANS=True
export LARAVEL_SPANNER_EMULATOR_HOST="${LARAVEL_SPANNER_EMULATOR_HOST-spanner-emulator:9010}"
export LARAVEL_SPANNER_PROJECT_ID="${LARAVEL_SPANNER_PROJECT_ID:-test-project}"
export LARAVEL_SPANNER_INSTANCE_ID="${LARAVEL_SPANNER_INSTANCE_ID:-laravel-bench-instance}"
export LARAVEL_SPANNER_DATABASE_ID="${LARAVEL_SPANNER_DATABASE_ID:-laravel-bench-db}"
export LARAVEL_SPANNER_MIN_SESSIONS="${LARAVEL_SPANNER_MIN_SESSIONS:-16}"

mkdir -p "$log_dir"
if [[ "${BENCH_LOG_CAPTURED:-}" != "1" ]]; then
    export BENCH_LOG_CAPTURED=1
    exec > >(tee -a "$log_dir/runner.log") 2>&1
fi

printf 'log_dir=%s\n' "$log_dir"
printf 'requests=%s actions=%s project=%s instance=%s database=%s emulator_host=%s min_sessions=%s\n' \
    "$requests" \
    "${actions[*]}" \
    "$LARAVEL_SPANNER_PROJECT_ID" \
    "$LARAVEL_SPANNER_INSTANCE_ID" \
    "$LARAVEL_SPANNER_DATABASE_ID" \
    "${LARAVEL_SPANNER_EMULATOR_HOST:-<cloud>}" \
    "$LARAVEL_SPANNER_MIN_SESSIONS"

ensure_app_dependencies() {
    if [[ -f "$app_dir/vendor/autoload.php" ]]; then
        return
    fi

    docker compose run --rm dev composer \
        --working-dir="/workspace/$app_dir" \
        install --no-interaction --prefer-dist
}

run_variant() {
    local variant="$1"
    local service="$2"
    local action="$3"

    if [[ "$LARAVEL_SPANNER_EMULATOR_HOST" != "" ]]; then
        docker compose up -d spanner-emulator
    fi
    docker compose up -d --force-recreate "$service"
    fastcgi_loop "$service" "$action" 1 >/dev/null

    local ticks
    ticks="$(docker compose exec -T "$service" sh -lc 'getconf CLK_TCK')"

    local before_worker
    before_worker="$(worker_ticks "$service")"
    local before_cgroup
    before_cgroup="$(cgroup_cpu_us "$service")"

    fastcgi_loop "$service" "$action" "$requests" >/dev/null

    local after_worker
    after_worker="$(worker_ticks "$service")"
    local after_cgroup
    after_cgroup="$(cgroup_cpu_us "$service")"

    awk \
        -v requests="$requests" \
        -v ticks="$ticks" \
        -v before_worker="$before_worker" \
        -v after_worker="$after_worker" \
        -v before_cgroup="$before_cgroup" \
        -v after_cgroup="$after_cgroup" \
        -v variant="$variant" \
        -v action="$action" \
        'BEGIN {
            worker_cpu_us = ((after_worker - before_worker) / ticks) * 1000000.0;
            cgroup_cpu_us = after_cgroup - before_cgroup;
            printf("%-12s %-24s %8d %20.1f %20.1f\n", variant, action, requests, worker_cpu_us / requests, cgroup_cpu_us / requests);
        }'
}

fastcgi_loop() {
    local service="$1"
    local action="$2"
    local count="$3"
    docker compose run --rm \
        -e "SCRIPT_FILENAME=/workspace/tools/benchmark/laravel-spanner-app/public/index.php" \
        -e "DOCUMENT_ROOT=/workspace/tools/benchmark/laravel-spanner-app/public" \
        -e "REQUEST_METHOD=GET" \
        -e "REQUEST_URI=/bench?action=$action" \
        -e "QUERY_STRING=action=$action" \
        -e "SPANNER_EMULATOR_HOST=$LARAVEL_SPANNER_EMULATOR_HOST" \
        -e "DB_SPANNER_PROJECT_ID=$LARAVEL_SPANNER_PROJECT_ID" \
        -e "DB_SPANNER_INSTANCE_ID=$LARAVEL_SPANNER_INSTANCE_ID" \
        -e "DB_SPANNER_DATABASE_ID=$LARAVEL_SPANNER_DATABASE_ID" \
        -e "FPM_SERVICE=$service" \
        -e "REQUESTS=$count" \
        dev sh -lc '
            i=0
            while [ "$i" -lt "$REQUESTS" ]; do
                cgi-fcgi -bind -connect "$FPM_SERVICE:9000" >/dev/null || exit 1
                i=$((i + 1))
            done
        '
}

worker_ticks() {
    local service="$1"
    docker compose exec -T "$service" sh -lc "total=0; for path in /proc/[0-9]*/cmdline; do cmd=\$(tr '\\0' ' ' < \"\$path\"); case \"\$cmd\" in 'php-fpm: pool www'*) pid=\$(basename \"\$(dirname \"\$path\")\"); ticks=\$(awk '{print \$14 + \$15}' /proc/\$pid/stat); total=\$((total + ticks));; esac; done; echo \$total"
}

cgroup_cpu_us() {
    local service="$1"
    docker compose exec -T "$service" sh -lc "awk '/^usage_usec / {print \$2}' /sys/fs/cgroup/cpu.stat"
}

ensure_app_dependencies

printf "%-12s %-24s %8s %20s %20s\n" variant measurement requests worker_cpu_us/request cgroup_cpu_us/request
printf "%80s\n" "" | tr " " "-"

for action in "${actions[@]}"; do
    run_variant native fpm-lifecycle-16 "$action"
    run_variant ext-grpc fpm-ext-grpc-16 "$action"
done
