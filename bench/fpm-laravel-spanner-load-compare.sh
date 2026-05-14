#!/usr/bin/env bash
#
# Measure Laravel + colopl/laravel-spanner through nginx + php-fpm under load.
#
# Usage:
#   ./bench/fpm-laravel-spanner-load-compare.sh [requests] [concurrency]
#
# requests must be divisible by concurrency. hey distributes work evenly across
# workers and otherwise runs fewer requests than requested.
#
# BENCH_ACTIONS can override the default action list. Set
# LARAVEL_SPANNER_EMULATOR_HOST= to use Cloud Spanner instead of the local
# emulator.
#
set -euo pipefail

cd "$(dirname "$0")/.."

requests="${1:-1024}"
concurrency="${2:-16}"
IFS=" " read -r -a actions <<< "${BENCH_ACTIONS:-transaction_select2_update1_insert1}"
IFS=" " read -r -a variants <<< "${BENCH_VARIANTS:-native ext-grpc}"
app_dir="tools/benchmark/laravel-spanner-app"
run_id="${BENCH_RUN_ID:-$(date +%Y%m%d-%H%M%S)}"
log_dir="${BENCH_LOG_DIR:-var/bench-results/fpm-laravel-spanner-load-$run_id}"
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
if (( requests % concurrency != 0 )); then
    printf 'requests must be divisible by concurrency: requests=%s concurrency=%s\n' "$requests" "$concurrency" >&2
    exit 1
fi
printf 'requests=%s concurrency=%s actions=%s variants=%s project=%s instance=%s database=%s emulator_host=%s min_sessions=%s\n' \
    "$requests" \
    "$concurrency" \
    "${actions[*]}" \
    "${variants[*]}" \
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
    local fpm_service="$2"
    local nginx_service="$3"
    local action="$4"

    if [[ "$LARAVEL_SPANNER_EMULATOR_HOST" != "" ]]; then
        docker compose up -d spanner-emulator
    fi
    docker compose up -d --force-recreate "$fpm_service" "$nginx_service"
    wait_until_ready "$nginx_service"
    warm_fpm_workers "$fpm_service" "$nginx_service" "$action" "$variant"

    local before_cgroup
    before_cgroup="$(cgroup_cpu_us "$fpm_service")"

    local output
    output="$(docker compose run --rm loadgen \
        -n "$requests" \
        -c "$concurrency" \
        -disable-keepalive \
        "http://$nginx_service:8080/bench?action=$action")"
    printf '%s\n' "$output" > "$log_dir/hey-$variant-$action.log"
    local completed_requests
    completed_requests="$(assert_success_responses "$output" "$variant" "$action")"

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
        -v completed_requests="$completed_requests" \
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
            printf("%-12s %-24s %8d %8s %12.1f %10.1f %10.1f %10.1f %10.1f\n", variant, action, completed_requests, rps, cgroup_cpu_us / completed_requests, average_ms, p50_ms, p90_ms, max_ms);
        }'
}

fpm_worker_count() {
    local service="$1"
    docker compose exec -T "$service" sh -lc '
        count=0
        for path in /proc/[0-9]*/cmdline; do
            cmd=$(tr "\0" " " < "$path" 2>/dev/null || true)
            case "$cmd" in
                "php-fpm: pool www"*) count=$((count + 1)) ;;
            esac
        done
        printf "%s\n" "$count"
    '
}

warm_fpm_workers() {
    local fpm_service="$1"
    local nginx_service="$2"
    local action="$3"
    local variant="$4"
    local workers
    workers="$(fpm_worker_count "$fpm_service")"
    if [[ "$workers" -le 0 ]]; then
        workers=1
    fi
    local warmup_requests="${BENCH_FPM_WARMUP_REQUESTS:-$((workers * 4))}"
    local warmup_concurrency="${BENCH_FPM_WARMUP_CONCURRENCY:-$workers}"
    printf 'warmup variant=%s action=%s workers=%s requests=%s concurrency=%s\n' \
        "$variant" "$action" "$workers" "$warmup_requests" "$warmup_concurrency"
    docker compose run --rm loadgen \
        -n "$warmup_requests" \
        -c "$warmup_concurrency" \
        -disable-keepalive \
        "http://$nginx_service:8080/bench?action=$action" \
        > "$log_dir/warmup-$variant-$action.log"
}

assert_success_responses() {
    local output="$1"
    local variant="$2"
    local action="$3"
    local ok_count
    local non_200_count

    ok_count="$(printf '%s\n' "$output" | awk '/  \[200\]/ {print $2}')"
    non_200_count="$(printf '%s\n' "$output" | awk '/^  \[[0-9]+\]/{gsub(/\[/, "", $1); gsub(/\]/, "", $1); if ($1 != "200") total += $2} END {print total + 0}')"
    if [[ "${ok_count:-0}" != "$requests" || "$non_200_count" != "0" ]]; then
        printf 'unsuccessful response detected: variant=%s action=%s expected=%s actual_200=%s non_200=%s\n' \
            "$variant" "$action" "$requests" "${ok_count:-0}" "$non_200_count" >&2
        printf '%s\n' "$output" >&2
        return 1
    fi
    printf '%s\n' "$ok_count"
}

wait_until_ready() {
    local nginx_service="$1"
    local attempt
    for attempt in $(seq 1 60); do
        if docker compose run --rm dev curl --max-time 10 -fsS "http://$nginx_service:8080/bench?action=select_1row_10col" >/dev/null 2>&1; then
            return
        fi
        sleep 1
    done
    echo "failed to wait for $nginx_service" >&2
    return 1
}

cgroup_cpu_us() {
    local service="$1"
    docker compose exec -T "$service" sh -lc "awk '/^usage_usec / {print \$2}' /sys/fs/cgroup/cpu.stat"
}

ensure_app_dependencies

printf "%-12s %-24s %8s %8s %12s %10s %10s %10s %10s\n" variant measurement requests rps cpu_us/req avg_ms p50_ms p90_ms max_ms
printf "%100s\n" "" | tr " " "-"

for action in "${actions[@]}"; do
    for variant in "${variants[@]}"; do
        case "$variant" in
            native)
                run_variant native fpm-lifecycle-16 nginx-laravel-native "$action"
                ;;
            ext-grpc)
                run_variant ext-grpc fpm-ext-grpc-16 nginx-laravel-ext-grpc "$action"
                ;;
            *)
                echo "unknown BENCH_VARIANTS entry: $variant" >&2
                exit 1
                ;;
        esac
    done
done
