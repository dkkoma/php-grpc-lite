#!/usr/bin/env bash
#
# Compare unary request/response shapes observed from Spanner DML flow:
# BeginTransaction, ExecuteSql DML insert/update/delete, and Commit.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${BENCH_OUTPUT_DIR:-var/bench-results}"
duration="${DURATION:-1}"
warmup_calls="${WARMUP_CALLS:-3}"
max_calls="${MAX_CALLS:-0}"
include_franken="${INCLUDE_FRANKEN:-0}"
mkdir -p "$output_dir"

summary_tsv="$output_dir/phase2-spanner-dml-unary-shape-$timestamp.tsv"
docker_env=()
for env_name in \
    BENCH_OTEL_EXPORTER \
    BENCH_OTEL_EXPORTER_OTLP_ENDPOINT \
    OTEL_EXPORTER_OTLP_TRACES_ENDPOINT \
    OTEL_EXPORTER_OTLP_ENDPOINT
do
    if [[ -n "${!env_name:-}" ]]; then
        docker_env+=(-e "$env_name=${!env_name}")
    fi
done

metric() {
    local file="$1"
    local index="$2"
    local key="$3"
    jq -r --argjson index "$index" --arg key "$key" '.measurements[$index].metrics[$key].value // ""' "$file"
}

context() {
    local file="$1"
    local index="$2"
    local key="$3"
    jq -r --argjson index "$index" --arg key "$key" '.measurements[$index].attributes[$key] // ""' "$file"
}

append_results() {
    local implementation="$1"
    local variant="$2"
    local file="$3"
    local count
    count="$(jq '.measurements | length' "$file")"
    for index in $(seq 0 $((count - 1))); do
        local name request_bytes response_bytes calls cps p50_ns p99_ns
        name="$(jq -r --argjson index "$index" '.measurements[$index].name' "$file")"
        request_bytes="$(context "$file" "$index" request_bytes)"
        response_bytes="$(context "$file" "$index" response_bytes)"
        calls="$(metric "$file" "$index" calls_total)"
        cps="$(metric "$file" "$index" calls_per_second)"
        p50_ns="$(metric "$file" "$index" latency_p50_ns)"
        p99_ns="$(metric "$file" "$index" latency_p99_ns)"
        awk -v name="$name" -v implementation="$implementation" -v variant="$variant" \
            -v request_bytes="$request_bytes" -v response_bytes="$response_bytes" \
            -v calls="$calls" -v cps="$cps" -v p50_ns="$p50_ns" -v p99_ns="$p99_ns" -v file="$file" \
            'BEGIN { printf "%s\t%s\t%s\t%s\t%s\t%s\t%.1f\t%.1f\t%.1f\t%s\n", name, implementation, variant, request_bytes, response_bytes, calls, cps, p50_ns / 1000.0, p99_ns / 1000.0, file }' >> "$summary_tsv"
    done
}

printf "case\timplementation\tvariant\trequest_bytes\tresponse_bytes\tcalls\tcalls_per_second\tp50_us\tp99_us\tjson\n" > "$summary_tsv"

native_json="$output_dir/phase2-spanner-dml-unary-shape-$timestamp-native.json"
docker compose run --rm "${docker_env[@]+"${docker_env[@]}"}" dev sh -lc \
    "php -d extension=/workspace/ext/grpc/modules/grpc.so tools/phase2/unary-shape.php --suite=spanner-dml-unary-shape --implementation=php-grpc-lite --autoload=vendor/autoload.php --output='$native_json' --duration='$duration' --warmup-calls='$warmup_calls' --max-calls='$max_calls'"
append_results php-grpc-lite native "$native_json"

if [[ "$include_franken" == "1" ]]; then
    franken_json="$output_dir/phase2-spanner-dml-unary-shape-$timestamp-franken-go.json"
    docker compose run --rm "${docker_env[@]+"${docker_env[@]}"}" dev-franken-grpc-go tools/frankenphp-grpc-lite-run.sh tools/phase2/unary-shape.php \
        --suite=spanner-dml-unary-shape \
        --implementation=php-grpc-lite \
        --transport=franken-go \
        --autoload=vendor/autoload.php \
        --output="$franken_json" \
        --duration="$duration" \
        --warmup-calls="$warmup_calls" \
        --max-calls="$max_calls"
    append_results php-grpc-lite franken-go "$franken_json"
fi

ext_json="$output_dir/phase2-spanner-dml-unary-shape-$timestamp-ext-grpc.json"
docker compose run --rm "${docker_env[@]+"${docker_env[@]}"}" dev-ext-grpc php tools/phase2/unary-shape.php \
    --suite=spanner-dml-unary-shape \
    --implementation=ext-grpc \
    --autoload=vendor/autoload.php \
    --output="$ext_json" \
    --duration="$duration" \
    --warmup-calls="$warmup_calls" \
    --max-calls="$max_calls"
append_results ext-grpc c-core "$ext_json"

echo
echo "Summary TSV: $summary_tsv"
column -t -s $'\t' "$summary_tsv" || cat "$summary_tsv"
