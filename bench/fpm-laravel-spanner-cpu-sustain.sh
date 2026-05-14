#!/usr/bin/env bash
#
# Sample php-fpm CPU state while Laravel + colopl/laravel-spanner is under
# sustained HTTP load.
#
# Usage:
#   ./bench/fpm-laravel-spanner-cpu-sustain.sh [concurrency] [duration] [variant]
#
# variant: native | ext-grpc
#
set -euo pipefail

cd "$(dirname "$0")/.."

concurrency="${1:-32}"
duration="${2:-30s}"
variant="${3:-native}"
action="${BENCH_ACTION:-select_1row_10col}"
sample_interval="${BENCH_SAMPLE_INTERVAL:-1}"
app_dir="tools/benchmark/laravel-spanner-app"
run_id="${BENCH_RUN_ID:-$(date +%Y%m%d-%H%M%S)}"
log_dir="${BENCH_LOG_DIR:-var/bench-results/fpm-laravel-spanner-cpu-sustain-$run_id}"
export COMPOSE_IGNORE_ORPHANS=True
export LARAVEL_SPANNER_EMULATOR_HOST="${LARAVEL_SPANNER_EMULATOR_HOST-spanner-emulator:9010}"
export LARAVEL_SPANNER_PROJECT_ID="${LARAVEL_SPANNER_PROJECT_ID:-test-project}"
export LARAVEL_SPANNER_INSTANCE_ID="${LARAVEL_SPANNER_INSTANCE_ID:-laravel-bench-instance}"
export LARAVEL_SPANNER_DATABASE_ID="${LARAVEL_SPANNER_DATABASE_ID:-laravel-bench-db}"
export LARAVEL_SPANNER_MIN_SESSIONS="${LARAVEL_SPANNER_MIN_SESSIONS:-16}"

case "$variant" in
    native)
        fpm_service="fpm-lifecycle-16"
        nginx_service="nginx-laravel-native"
        ;;
    ext-grpc)
        fpm_service="fpm-ext-grpc-16"
        nginx_service="nginx-laravel-ext-grpc"
        ;;
    *)
        echo "unknown variant: $variant" >&2
        exit 1
        ;;
esac

mkdir -p "$log_dir"
exec > >(tee -a "$log_dir/runner.log") 2>&1

printf 'log_dir=%s\n' "$log_dir"
printf 'variant=%s action=%s concurrency=%s duration=%s sample_interval=%s project=%s instance=%s database=%s emulator_host=%s min_sessions=%s\n' \
    "$variant" \
    "$action" \
    "$concurrency" \
    "$duration" \
    "$sample_interval" \
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

start_services() {
    if [[ "$LARAVEL_SPANNER_EMULATOR_HOST" != "" ]]; then
        docker compose up -d spanner-emulator
    fi

    if [[ "${BENCH_RECREATE:-0}" == "1" ]]; then
        docker compose up -d --force-recreate "$fpm_service" "$nginx_service"
    else
        docker compose up -d "$fpm_service" "$nginx_service"
    fi

    wait_until_ready
}

wait_until_ready() {
    local attempt
    for attempt in $(seq 1 60); do
        if docker compose run --rm dev curl --max-time 10 -fsS "http://$nginx_service:8080/bench?action=$action" >/dev/null 2>&1; then
            return
        fi
        sleep 1
    done
    echo "failed to wait for $nginx_service" >&2
    return 1
}

sample_fpm_state() {
    docker compose exec -T "$fpm_service" sh -lc '
        usage_usec=0
        nr_periods=0
        nr_throttled=0
        throttled_usec=0
        while read -r key value; do
            case "$key" in
                usage_usec) usage_usec="$value" ;;
                nr_periods) nr_periods="$value" ;;
                nr_throttled) nr_throttled="$value" ;;
                throttled_usec) throttled_usec="$value" ;;
            esac
        done < /sys/fs/cgroup/cpu.stat

        worker_count=0
        worker_ticks=0
        voluntary_ctxt=0
        nonvoluntary_ctxt=0
        for path in /proc/[0-9]*/cmdline; do
            cmd=$(tr "\0" " " < "$path" 2>/dev/null || true)
            case "$cmd" in
                "php-fpm: pool www"*)
                    pid=$(basename "$(dirname "$path")")
                    ticks=$(awk "{print \$14 + \$15}" "/proc/$pid/stat" 2>/dev/null || echo 0)
                    vcsw=$(awk "/^voluntary_ctxt_switches:/ {print \$2}" "/proc/$pid/status" 2>/dev/null || echo 0)
                    nvcsw=$(awk "/^nonvoluntary_ctxt_switches:/ {print \$2}" "/proc/$pid/status" 2>/dev/null || echo 0)
                    worker_count=$((worker_count + 1))
                    worker_ticks=$((worker_ticks + ticks))
                    voluntary_ctxt=$((voluntary_ctxt + vcsw))
                    nonvoluntary_ctxt=$((nonvoluntary_ctxt + nvcsw))
                    ;;
            esac
        done

        pressure_some_avg10=""
        pressure_full_avg10=""
        if [ -r /sys/fs/cgroup/cpu.pressure ]; then
            pressure_some_avg10=$(awk "/^some / {for (i=1; i<=NF; i++) if (\$i ~ /^avg10=/) {sub(/^avg10=/, \"\", \$i); print \$i}}" /sys/fs/cgroup/cpu.pressure)
            pressure_full_avg10=$(awk "/^full / {for (i=1; i<=NF; i++) if (\$i ~ /^avg10=/) {sub(/^avg10=/, \"\", \$i); print \$i}}" /sys/fs/cgroup/cpu.pressure)
        fi

        printf "%s %s %s %s %s %s %s %s %s\n" \
            "$usage_usec" \
            "$nr_periods" \
            "$nr_throttled" \
            "$throttled_usec" \
            "$worker_count" \
            "$worker_ticks" \
            "$voluntary_ctxt" \
            "$nonvoluntary_ctxt" \
            "${pressure_some_avg10:-0}:${pressure_full_avg10:-0}"
    '
}

write_sample_header() {
    printf 'epoch_s,elapsed_s,usage_usec,delta_usage_usec,cpu_cores,nr_periods,delta_nr_periods,nr_throttled,delta_nr_throttled,throttled_usec,delta_throttled_usec,worker_count,worker_ticks,delta_worker_ticks,voluntary_ctxt,delta_voluntary_ctxt,nonvoluntary_ctxt,delta_nonvoluntary_ctxt,cpu_pressure_some_avg10,cpu_pressure_full_avg10\n' \
        > "$log_dir/fpm-cpu-samples.csv"
}

append_sample() {
    local start_s="$1"
    local prev_s="$2"
    local prev_usage="$3"
    local prev_periods="$4"
    local prev_throttled="$5"
    local prev_throttled_usec="$6"
    local prev_ticks="$7"
    local prev_vcsw="$8"
    local prev_nvcsw="$9"
    local epoch_s usage periods throttled throttled_usec worker_count ticks vcsw nvcsw pressure pressure_some pressure_full

    epoch_s="$(date +%s)"
    read -r usage periods throttled throttled_usec worker_count ticks vcsw nvcsw pressure < <(sample_fpm_state)
    pressure_some="${pressure%%:*}"
    pressure_full="${pressure##*:}"

    awk \
        -v epoch_s="$epoch_s" \
        -v start_s="$start_s" \
        -v prev_s="$prev_s" \
        -v usage="$usage" \
        -v prev_usage="$prev_usage" \
        -v periods="$periods" \
        -v prev_periods="$prev_periods" \
        -v throttled="$throttled" \
        -v prev_throttled="$prev_throttled" \
        -v throttled_usec="$throttled_usec" \
        -v prev_throttled_usec="$prev_throttled_usec" \
        -v worker_count="$worker_count" \
        -v ticks="$ticks" \
        -v prev_ticks="$prev_ticks" \
        -v vcsw="$vcsw" \
        -v prev_vcsw="$prev_vcsw" \
        -v nvcsw="$nvcsw" \
        -v prev_nvcsw="$prev_nvcsw" \
        -v pressure_some="$pressure_some" \
        -v pressure_full="$pressure_full" \
        'BEGIN {
            elapsed = epoch_s - start_s;
            interval = epoch_s - prev_s;
            if (interval <= 0) interval = 1;
            delta_usage = usage - prev_usage;
            cpu_cores = delta_usage / (interval * 1000000.0);
            printf("%d,%d,%d,%d,%.3f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,%s\n",
                epoch_s,
                elapsed,
                usage,
                delta_usage,
                cpu_cores,
                periods,
                periods - prev_periods,
                throttled,
                throttled - prev_throttled,
                throttled_usec,
                throttled_usec - prev_throttled_usec,
                worker_count,
                ticks,
                ticks - prev_ticks,
                vcsw,
                vcsw - prev_vcsw,
                nvcsw,
                nvcsw - prev_nvcsw,
                pressure_some,
                pressure_full);
        }' >> "$log_dir/fpm-cpu-samples.csv"

    printf '%s %s %s %s %s %s %s %s\n' \
        "$epoch_s" "$usage" "$periods" "$throttled" "$throttled_usec" "$ticks" "$vcsw" "$nvcsw"
}

summarize_samples() {
    awk -F, '
        NR == 1 { next }
        {
            n++;
            cpu_sum += $5;
            if (n == 1 || $5 < cpu_min) cpu_min = $5;
            if (n == 1 || $5 > cpu_max) cpu_max = $5;
            throttled_periods += $9;
            throttled_usec += $11;
            worker_ticks += $14;
            vcsw += $16;
            nvcsw += $18;
            last_elapsed = $2;
        }
        END {
            if (n == 0) exit;
            printf("samples=%d duration_s=%d cpu_cores_avg=%.3f cpu_cores_min=%.3f cpu_cores_max=%.3f throttled_periods=%d throttled_usec=%d worker_ticks=%d voluntary_ctxt=%d nonvoluntary_ctxt=%d\n",
                n, last_elapsed, cpu_sum / n, cpu_min, cpu_max, throttled_periods, throttled_usec, worker_ticks, vcsw, nvcsw);
        }
    ' "$log_dir/fpm-cpu-samples.csv" | tee "$log_dir/fpm-cpu-summary.txt"
}

ensure_app_dependencies
start_services

write_sample_header
read -r start_usage start_periods start_throttled start_throttled_usec start_worker_count start_ticks start_vcsw start_nvcsw start_pressure < <(sample_fpm_state)
start_s="$(date +%s)"
prev_s="$start_s"
prev_usage="$start_usage"
prev_periods="$start_periods"
prev_throttled="$start_throttled"
prev_throttled_usec="$start_throttled_usec"
prev_ticks="$start_ticks"
prev_vcsw="$start_vcsw"
prev_nvcsw="$start_nvcsw"

docker compose run --rm loadgen \
    -z "$duration" \
    -c "$concurrency" \
    -disable-keepalive \
    "http://$nginx_service:8080/bench?action=$action" \
    > "$log_dir/hey-$variant-$action.log" &
loadgen_pid="$!"

while kill -0 "$loadgen_pid" >/dev/null 2>&1; do
    sleep "$sample_interval"
    read -r prev_s prev_usage prev_periods prev_throttled prev_throttled_usec prev_ticks prev_vcsw prev_nvcsw < <(
        append_sample "$start_s" "$prev_s" "$prev_usage" "$prev_periods" "$prev_throttled" "$prev_throttled_usec" "$prev_ticks" "$prev_vcsw" "$prev_nvcsw"
    )
done
wait "$loadgen_pid"

summarize_samples
