#!/usr/bin/env bash
#
# Measure php-fpm worker CPU ticks for FastCGI request lifecycle.
#
# Usage:
#   ./bench/fpm-cpu-compare.sh [requests]
#
set -euo pipefail

cd "$(dirname "$0")/.."

requests="${1:-1000}"
cases=(small_unary_100b small_streaming_1x100b)
export COMPOSE_IGNORE_ORPHANS=True

run_variant() {
    local variant="$1"
    local service="$2"
    local case_name="$3"

    docker compose up -d "$service"

    # Ensure the static worker is spawned and has a live connection once.
    fastcgi_loop "$service" "$case_name" 1 >/dev/null

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

    fastcgi_loop "$service" "$case_name" "$requests" >/dev/null

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
        -v case_name="$case_name" \
        'BEGIN {
            worker_cpu_us = ((after_worker - before_worker) / ticks) * 1000000.0;
            cgroup_cpu_us = after_cgroup - before_cgroup;
            printf("%-12s %-24s %8d %20.1f %20.1f\n", variant, case_name, requests, worker_cpu_us / requests, cgroup_cpu_us / requests);
        }'
}

fastcgi_loop() {
    local service="$1"
    local case_name="$2"
    local count="$3"
    docker compose run --rm \
        -e "SCRIPT_FILENAME=/workspace/tools/benchmark/fpm-request.php" \
        -e "REQUEST_METHOD=GET" \
        -e "BENCH_CASE=$case_name" \
        -e "BENCH_TARGET=test-server:50051" \
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

printf "%-12s %-24s %8s %20s %20s\n" variant measurement requests worker_cpu_us/request cgroup_cpu_us/request
printf "%80s\n" "" | tr " " "-"

for case_name in "${cases[@]}"; do
    run_variant native fpm-lifecycle "$case_name"
    run_variant ext-grpc fpm-ext-grpc "$case_name"
done
