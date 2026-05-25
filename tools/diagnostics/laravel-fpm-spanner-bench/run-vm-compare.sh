#!/usr/bin/env bash
#
# Run Laravel + colopl/laravel-spanner through nginx + php-fpm on a GCP VM.
# Images are expected to be prebuilt in GHCR. The VM must use metadata server
# ADC; do not pass GOOGLE_APPLICATION_CREDENTIALS.
#
# Usage:
#   ./run-vm-compare.sh [requests] [concurrency]
#
set -euo pipefail

requests="${1:-192}"
concurrency="${2:-16}"
actions="${BENCH_ACTIONS:-transaction_select2_update1_insert1 select_1row_10col}"
variants="${BENCH_VARIANTS:-lite official}"
image_repo="${IMAGE_REPO:-ghcr.io/dkkoma/php-grpc-lite-spanner-repro}"
tag_suffix="${IMAGE_TAG_SUFFIX:-}"
run_id="${BENCH_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
out_dir="${BENCH_OUT_DIR:-$PWD/results/laravel-fpm-spanner-$run_id}"
network="laravel-fpm-spanner-$run_id"

project="${LARAVEL_SPANNER_PROJECT_ID:-vast-falcon-165704}"
instance="${LARAVEL_SPANNER_INSTANCE_ID:-bench}"
database="${LARAVEL_SPANNER_DATABASE_ID:-laravel-bench-db}"
min_sessions="${LARAVEL_SPANNER_MIN_SESSIONS:-16}"
fpm_cpus="${BENCH_FPM_CPUS:-2.0}"
warmup_requests="${BENCH_FPM_WARMUP_REQUESTS:-64}"
warmup_concurrency="${BENCH_FPM_WARMUP_CONCURRENCY:-16}"

mkdir -p "$out_dir"
export DOCKER_CONFIG="${DOCKER_CONFIG:-$out_dir/docker-config}"
mkdir -p "$DOCKER_CONFIG"
exec > >(tee -a "$out_dir/runner.log") 2>&1

if (( requests % concurrency != 0 )); then
    echo "requests must be divisible by concurrency: requests=$requests concurrency=$concurrency" >&2
    exit 1
fi

image_tag() {
    local name="$1"
    if [[ "$tag_suffix" == "" ]]; then
        printf '%s:laravel-fpm-%s' "$image_repo" "$name"
    else
        printf '%s:laravel-fpm-%s-%s' "$image_repo" "$name" "$tag_suffix"
    fi
}

cleanup() {
    docker rm -f laravel-fpm laravel-nginx >/dev/null 2>&1 || true
    docker network rm "$network" >/dev/null 2>&1 || true
}
trap cleanup EXIT

cgroup_cpu_us() {
    docker exec laravel-fpm sh -lc "awk '/^usage_usec / {print \$2}' /sys/fs/cgroup/cpu.stat"
}

wait_until_ready() {
    local action="$1"
    local attempt
    for attempt in $(seq 1 90); do
        local output
        output="$(docker run --rm --network "$network" "$(image_tag loadgen)" \
            -n 1 -c 1 -disable-keepalive \
            "http://laravel-nginx:8080/bench?action=$action" 2>/dev/null || true)"
        if printf '%s\n' "$output" | grep -q '  \[200\][[:space:]]\+1 responses'; then
            return
        fi
        sleep 1
    done
    echo "failed to wait for laravel-nginx" >&2
    docker logs laravel-fpm >&2 || true
    docker logs laravel-nginx >&2 || true
    return 1
}

assert_success_responses() {
    local output="$1"
    local ok_count non_200_count
    ok_count="$(printf '%s\n' "$output" | awk '/  \[200\]/ {print $2}')"
    non_200_count="$(printf '%s\n' "$output" | awk '/^  \[[0-9]+\]/{gsub(/\[/, "", $1); gsub(/\]/, "", $1); if ($1 != "200") total += $2} END {print total + 0}')"
    if [[ "${ok_count:-0}" != "$requests" || "$non_200_count" != "0" ]]; then
        printf 'unsuccessful response detected: expected=%s actual_200=%s non_200=%s\n' \
            "$requests" "${ok_count:-0}" "$non_200_count" >&2
        printf '%s\n' "$output" >&2
        return 1
    fi
    printf '%s\n' "$ok_count"
}

run_variant() {
    local variant="$1"
    local action="$2"

    cleanup
    docker network create "$network" >/dev/null

    docker run -d --name laravel-fpm \
        --network "$network" \
        --cpus "$fpm_cpus" \
        --add-host metadata.google.internal:169.254.169.254 \
        -e GOOGLE_CLOUD_PROJECT="$project" \
        -e DB_SPANNER_PROJECT_ID="$project" \
        -e DB_SPANNER_INSTANCE_ID="$instance" \
        -e DB_SPANNER_DATABASE_ID="$database" \
        -e LARAVEL_SPANNER_MIN_SESSIONS="$min_sessions" \
        -e SPANNER_EMULATOR_HOST= \
        "$(image_tag "$variant")" >/dev/null

    docker run -d --name laravel-nginx \
        --network "$network" \
        -e FPM_UPSTREAM=laravel-fpm \
        -e SPANNER_EMULATOR_HOST= \
        -e DB_SPANNER_PROJECT_ID="$project" \
        -e DB_SPANNER_INSTANCE_ID="$instance" \
        -e DB_SPANNER_DATABASE_ID="$database" \
        "$(image_tag nginx)" >/dev/null

    wait_until_ready "$action"

    docker run --rm --network "$network" "$(image_tag loadgen)" \
        -n "$warmup_requests" \
        -c "$warmup_concurrency" \
        -disable-keepalive \
        "http://laravel-nginx:8080/bench?action=$action" \
        > "$out_dir/warmup-$variant-$action.log"

    local before_cgroup after_cgroup output completed_requests
    before_cgroup="$(cgroup_cpu_us)"
    output="$(docker run --rm --network "$network" "$(image_tag loadgen)" \
        -n "$requests" \
        -c "$concurrency" \
        -disable-keepalive \
        "http://laravel-nginx:8080/bench?action=$action")"
    after_cgroup="$(cgroup_cpu_us)"

    printf '%s\n' "$output" > "$out_dir/hey-$variant-$action.log"
    docker logs laravel-fpm > "$out_dir/fpm-$variant-$action.log" 2>&1 || true
    docker logs laravel-nginx > "$out_dir/nginx-$variant-$action.log" 2>&1 || true
    completed_requests="$(assert_success_responses "$output")"

    local rps average_ms p50_ms p90_ms p99_ms max_ms
    rps="$(printf '%s\n' "$output" | awk '/Requests\/sec:/ {print $2}')"
    average_ms="$(printf '%s\n' "$output" | awk '/Average:/ {printf "%.3f", $2 * 1000}')"
    p50_ms="$(printf '%s\n' "$output" | awk '$1 ~ /^50%+$/ {printf "%.3f", $3 * 1000}')"
    p90_ms="$(printf '%s\n' "$output" | awk '$1 ~ /^90%+$/ {printf "%.3f", $3 * 1000}')"
    p99_ms="$(printf '%s\n' "$output" | awk '$1 ~ /^99%+$/ {printf "%.3f", $3 * 1000}')"
    max_ms="$(printf '%s\n' "$output" | awk '/Slowest:/ {printf "%.3f", $2 * 1000}')"

    awk \
        -v variant="$variant" \
        -v action="$action" \
        -v completed_requests="$completed_requests" \
        -v before_cgroup="$before_cgroup" \
        -v after_cgroup="$after_cgroup" \
        -v rps="$rps" \
        -v average_ms="$average_ms" \
        -v p50_ms="$p50_ms" \
        -v p90_ms="$p90_ms" \
        -v p99_ms="$p99_ms" \
        -v max_ms="$max_ms" \
        'BEGIN {
            cgroup_cpu_us = after_cgroup - before_cgroup;
            printf("%-10s %-34s %8d %9s %12.1f %10.1f %10.1f %10.1f %10.1f %10.1f\n",
                variant, action, completed_requests, rps, cgroup_cpu_us / completed_requests,
                average_ms, p50_ms, p90_ms, p99_ms, max_ms);
        }'
}

echo "run_id=$run_id"
echo "out_dir=$out_dir"
echo "image_repo=$image_repo tag_suffix=${tag_suffix:-<none>}"
echo "project=$project instance=$instance database=$database auth=metadata-adc min_sessions=$min_sessions fpm_cpus=$fpm_cpus"
echo "requests=$requests concurrency=$concurrency actions=$actions variants=$variants"

for image in lite official nginx loadgen; do
    docker pull "$(image_tag "$image")"
done

printf "%-10s %-34s %8s %9s %12s %10s %10s %10s %10s %10s\n" variant action requests rps cpu_us/req avg_ms p50_ms p90_ms p99_ms max_ms
printf "%129s\n" "" | tr " " "-"

for action in $actions; do
    for variant in $variants; do
        run_variant "$variant" "$action"
    done
done
