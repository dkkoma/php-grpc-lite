#!/usr/bin/env bash
#
# Profile actual php-fpm worker requests with callgrind.
#
# Usage:
#   ./bench/fpm-laravel-spanner-callgrind-worker.sh [action] [requests] [concurrency]
#
set -euo pipefail

cd "$(dirname "$0")/.."

action="${1:-select_1row_10col}"
requests="${2:-10}"
concurrency="${3:-1}"
run_id="${BENCH_RUN_ID:-$(date +%Y%m%d-%H%M%S)}"
log_dir="${BENCH_LOG_DIR:-var/bench-results/fpm-laravel-spanner-callgrind-$run_id}"
export COMPOSE_IGNORE_ORPHANS=True
export FPM_CALLGRIND_LOG_DIR="$log_dir"
export LARAVEL_SPANNER_EMULATOR_HOST="${LARAVEL_SPANNER_EMULATOR_HOST-spanner-emulator:9010}"
export LARAVEL_SPANNER_PROJECT_ID="${LARAVEL_SPANNER_PROJECT_ID:-test-project}"
export LARAVEL_SPANNER_INSTANCE_ID="${LARAVEL_SPANNER_INSTANCE_ID:-laravel-bench-instance}"
export LARAVEL_SPANNER_DATABASE_ID="${LARAVEL_SPANNER_DATABASE_ID:-laravel-bench-db}"
export LARAVEL_SPANNER_MIN_SESSIONS="${LARAVEL_SPANNER_MIN_SESSIONS:-16}"

mkdir -p "$log_dir"
exec > >(tee -a "$log_dir/runner.log") 2>&1

printf 'log_dir=%s\n' "$log_dir"
printf 'action=%s requests=%s concurrency=%s project=%s instance=%s database=%s emulator_host=%s min_sessions=%s\n' \
    "$action" \
    "$requests" \
    "$concurrency" \
    "$LARAVEL_SPANNER_PROJECT_ID" \
    "$LARAVEL_SPANNER_INSTANCE_ID" \
    "$LARAVEL_SPANNER_DATABASE_ID" \
    "${LARAVEL_SPANNER_EMULATOR_HOST:-<cloud>}" \
    "$LARAVEL_SPANNER_MIN_SESSIONS"

cleanup() {
    docker compose stop nginx-laravel-callgrind fpm-lifecycle-callgrind >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker compose up -d --force-recreate fpm-lifecycle-callgrind nginx-laravel-callgrind

for attempt in $(seq 1 120); do
    if docker compose run --rm dev curl --max-time 30 -fsS "http://nginx-laravel-callgrind:8080/bench?action=$action" >/dev/null 2>&1; then
        break
    fi
    if [[ "$attempt" == "120" ]]; then
        echo 'failed to wait for callgrind FPM service' >&2
        docker compose logs --no-color fpm-lifecycle-callgrind nginx-laravel-callgrind | tail -200 >&2 || true
        exit 1
    fi
    sleep 1
done

# Warm one additional request. callgrind_control is intentionally not used here:
# in php-fpm master/worker mode it can make Valgrind lose its vgdb FIFO and exit
# before dumping usable worker data. The resulting profile includes FPM request
# warmup, but it is stable and still captures the real FastCGI/FPM execution path.
docker compose run --rm dev curl --max-time 30 -fsS "http://nginx-laravel-callgrind:8080/bench?action=$action" >/dev/null

docker compose run --rm loadgen \
    -n "$requests" \
    -c "$concurrency" \
    -disable-keepalive \
    "http://nginx-laravel-callgrind:8080/bench?action=$action" \
    > "$log_dir/hey-$action.log"

docker compose stop nginx-laravel-callgrind fpm-lifecycle-callgrind
sleep 2

shopt -s nullglob
callgrind_files=("$log_dir"/callgrind.out.*)
if (( ${#callgrind_files[@]} == 0 )); then
    echo 'no callgrind output files found' >&2
    docker compose logs --no-color fpm-lifecycle-callgrind | tail -200 >&2 || true
    exit 1
fi

for file in "${callgrind_files[@]}"; do
    if [[ ! -s "$file" ]]; then
        echo "skip empty callgrind output: $file" >&2
        continue
    fi
    base="${file##*/}"
    docker compose run --rm fpm-lifecycle-profile sh -lc "callgrind_annotate --threshold=0.2 /workspace/$file > /workspace/$log_dir/$base.annotate.txt"
    docker compose run --rm fpm-lifecycle-profile sh -lc "callgrind_annotate --inclusive=yes --threshold=0.2 /workspace/$file > /workspace/$log_dir/$base.inclusive.txt"
done

printf '\nhey summary:\n'
rg -n 'Total:|Requests/sec|Average:|50%%|90%%|99%%|Status code' "$log_dir/hey-$action.log" || true
printf '\ncallgrind files:\n'
printf '%s\n' "${callgrind_files[@]}"
printf '\nfirst annotation:\n'
first_annotation="$(find "$log_dir" -maxdepth 1 -name 'callgrind.out.*.annotate.txt' -size +0 -print | sort | head -1)"
if [[ -n "$first_annotation" ]]; then
    sed -n '1,160p' "$first_annotation"
fi
