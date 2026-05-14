#!/usr/bin/env bash
#
# Measure Laravel + colopl/laravel-spanner through nginx + php-fpm under load.
#
# Usage:
#   ./bench/fpm-laravel-spanner-load-compare.sh [requests] [concurrency]
#
set -euo pipefail

cd "$(dirname "$0")/.."

requests="${1:-1000}"
concurrency="${2:-16}"
actions=(select_1row_10col dml_insert_10col dml_update_10col dml_delete_10col)
app_dir="tools/benchmark/laravel-spanner-app"
export COMPOSE_IGNORE_ORPHANS=True

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
    local fpm_service="$2"
    local nginx_service="$3"
    local action="$4"

    docker compose up -d spanner-emulator "$fpm_service" "$nginx_service"

    http_once "$nginx_service" setup >/dev/null
    warmup_workers "$nginx_service"

    local before_cgroup
    before_cgroup="$(cgroup_cpu_us "$fpm_service")"

    local output
    output="$(docker compose run --rm loadgen \
        -n "$requests" \
        -c "$concurrency" \
        -disable-keepalive \
        "http://$nginx_service:8080/bench?action=$action")"

    local after_cgroup
    after_cgroup="$(cgroup_cpu_us "$fpm_service")"

    local rps average_ms p50_ms p90_ms max_ms
    rps="$(printf '%s\n' "$output" | awk '/Requests\/sec:/ {print $2}')"
    average_ms="$(printf '%s\n' "$output" | awk '/Average:/ {printf "%.3f", $2 * 1000}')"
    p50_ms="$(printf '%s\n' "$output" | awk '/  50%% in / {printf "%.3f", $3 * 1000}')"
    p90_ms="$(printf '%s\n' "$output" | awk '/  90%% in / {printf "%.3f", $3 * 1000}')"
    max_ms="$(printf '%s\n' "$output" | awk '/Slowest:/ {printf "%.3f", $2 * 1000}')"

    awk \
        -v requests="$requests" \
        -v before_cgroup="$before_cgroup" \
        -v after_cgroup="$after_cgroup" \
        -v variant="$variant" \
        -v action="$action" \
        -v rps="$rps" \
        -v average_ms="$average_ms" \
        -v p50_ms="$p50_ms" \
        -v p90_ms="$p90_ms" \
        -v max_ms="$max_ms" \
        'BEGIN {
            cgroup_cpu_us = after_cgroup - before_cgroup;
            printf("%-12s %-24s %8d %8s %12.1f %10.1f %10.1f %10.1f %10.1f\n", variant, action, requests, rps, cgroup_cpu_us / requests, average_ms, p50_ms, p90_ms, max_ms);
        }'
}

http_once() {
    local nginx_service="$1"
    local action="$2"
    docker compose run --rm dev curl -fsS "http://$nginx_service:8080/bench?action=$action"
}

warmup_workers() {
    local nginx_service="$1"
    docker compose run --rm loadgen \
        -n 64 \
        -c 16 \
        -disable-keepalive \
        "http://$nginx_service:8080/bench?action=warmup" >/dev/null
}

cgroup_cpu_us() {
    local service="$1"
    docker compose exec -T "$service" sh -lc "awk '/^usage_usec / {print \$2}' /sys/fs/cgroup/cpu.stat"
}

ensure_app_dependencies

printf "%-12s %-24s %8s %8s %12s %10s %10s %10s %10s\n" variant measurement requests rps cpu_us/req avg_ms p50_ms p90_ms max_ms
printf "%100s\n" "" | tr " " "-"

for action in "${actions[@]}"; do
    run_variant native fpm-lifecycle-16 nginx-laravel-native "$action"
    run_variant ext-grpc fpm-ext-grpc-16 nginx-laravel-ext-grpc "$action"
done
