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
actions=(select_1row_10col dml_insert_10col dml_update_10col dml_delete_10col)
app_dir="tools/benchmark/laravel-spanner-app"
export COMPOSE_IGNORE_ORPHANS=True
export SPANNER_EMULATOR_HOST="${SPANNER_EMULATOR_HOST:-spanner-emulator:9010}"
export DB_SPANNER_PROJECT_ID="${DB_SPANNER_PROJECT_ID:-test-project}"
export DB_SPANNER_INSTANCE_ID="${DB_SPANNER_INSTANCE_ID:-laravel-bench-instance}"
export DB_SPANNER_DATABASE_ID="${DB_SPANNER_DATABASE_ID:-laravel-bench-db}"

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

    docker compose up -d spanner-emulator "$service"

    fastcgi_loop "$service" setup 1 >/dev/null
    fastcgi_loop "$service" warmup 1 >/dev/null

    local ticks
    ticks="$(docker compose exec -T "$service" sh -lc 'getconf CLK_TCK')"
    local worker_pid
    worker_pid="$(docker compose exec -T "$service" sh -lc "for path in /proc/[0-9]*/cmdline; do cmd=\$(tr '\\0' ' ' < \"\$path\"); case \"\$cmd\" in 'php-fpm: pool www'*) basename \"\$(dirname \"\$path\")\"; break;; esac; done")"
    if [[ -z "$worker_pid" ]]; then
        echo "failed to find php-fpm worker pid for $service" >&2
        exit 1
    fi

    local before_worker
    before_worker="$(worker_ticks "$service" "$worker_pid")"
    local before_cgroup
    before_cgroup="$(cgroup_cpu_us "$service")"

    fastcgi_loop "$service" "$action" "$requests" >/dev/null

    local after_worker
    after_worker="$(worker_ticks "$service" "$worker_pid")"
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
        -e "SCRIPT_FILENAME=/workspace/tools/benchmark/laravel-spanner-request.php" \
        -e "REQUEST_METHOD=GET" \
        -e "BENCH_ACTION=$action" \
        -e "SPANNER_EMULATOR_HOST=$SPANNER_EMULATOR_HOST" \
        -e "DB_SPANNER_PROJECT_ID=$DB_SPANNER_PROJECT_ID" \
        -e "DB_SPANNER_INSTANCE_ID=$DB_SPANNER_INSTANCE_ID" \
        -e "DB_SPANNER_DATABASE_ID=$DB_SPANNER_DATABASE_ID" \
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
    local worker_pid="$2"
    docker compose exec -T "$service" sh -lc "awk '{print \$14 + \$15}' /proc/$worker_pid/stat"
}

cgroup_cpu_us() {
    local service="$1"
    docker compose exec -T "$service" sh -lc "awk '/^usage_usec / {print \$2}' /sys/fs/cgroup/cpu.stat"
}

ensure_app_dependencies

printf "%-12s %-24s %8s %20s %20s\n" variant measurement requests worker_cpu_us/request cgroup_cpu_us/request
printf "%80s\n" "" | tr " " "-"

for action in "${actions[@]}"; do
    run_variant native fpm-lifecycle "$action"
    run_variant ext-grpc fpm-ext-grpc "$action"
done
