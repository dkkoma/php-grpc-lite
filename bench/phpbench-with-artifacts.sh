#!/usr/bin/env bash
#
# Run PHPBench inside a container and produce all artifacts before returning to
# the host. This avoids writing a log on the host and immediately asking a
# different container to parse it through a bind mount.
#
# Usage:
#   bench/phpbench-with-artifacts.sh \
#     --workdir=. \
#     --log=var/bench-results/example.log \
#     --json=var/bench-results/example.json \
#     --tsv=var/bench-results/example.tsv \
#     -- vendor/bin/phpbench run --report=aggregate
#
set -euo pipefail

workdir="."
log_path=""
json_path=""
tsv_path=""
baseline_path=""
suite=""
implementation=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --workdir)
            workdir="$2"
            shift 2
            ;;
        --workdir=*)
            workdir="${1#--workdir=}"
            shift
            ;;
        --log)
            log_path="$2"
            shift 2
            ;;
        --log=*)
            log_path="${1#--log=}"
            shift
            ;;
        --json)
            json_path="$2"
            shift 2
            ;;
        --json=*)
            json_path="${1#--json=}"
            shift
            ;;
        --tsv)
            tsv_path="$2"
            shift 2
            ;;
        --tsv=*)
            tsv_path="${1#--tsv=}"
            shift
            ;;
        --baseline)
            baseline_path="$2"
            shift 2
            ;;
        --baseline=*)
            baseline_path="${1#--baseline=}"
            shift
            ;;
        --suite)
            suite="$2"
            shift 2
            ;;
        --suite=*)
            suite="${1#--suite=}"
            shift
            ;;
        --implementation)
            implementation="$2"
            shift 2
            ;;
        --implementation=*)
            implementation="${1#--implementation=}"
            shift
            ;;
        --)
            shift
            break
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

if [[ "$log_path" == "" || "$json_path" == "" || "$tsv_path" == "" ]]; then
    echo "--log, --json, and --tsv are required" >&2
    exit 2
fi
if [[ $# -eq 0 ]]; then
    echo "phpbench command is required after --" >&2
    exit 2
fi

tmpdir="$(mktemp -d)"
tmp_log="$tmpdir/phpbench.log"
tmp_json="$tmpdir/phpbench.json"
tmp_tsv="$tmpdir/phpbench.tsv"
cleanup() {
    local status=$?
    if [[ "$status" != "0" && -s "$tmp_log" && "$log_path" != "" ]]; then
        cp "$tmp_log" "$log_path" 2>/dev/null || true
    fi
    rm -rf "$tmpdir"
    exit "$status"
}
trap cleanup EXIT

mkdir -p "$(dirname "$log_path")" "$(dirname "$json_path")" "$(dirname "$tsv_path")"

(
    cd "$workdir"
    "$@"
) | tee "$tmp_log"

php /workspace/tools/parse-phpbench-aggregate.php \
    --format=json \
    --output="$tmp_json" \
    "$tmp_log"
php /workspace/tools/parse-phpbench-aggregate.php \
    --format=tsv \
    --output="$tmp_tsv" \
    "$tmp_log"

cp "$tmp_log" "$log_path"
cp "$tmp_json" "$json_path"
cp "$tmp_tsv" "$tsv_path"

echo "  JSON: $json_path"
echo "  TSV: $tsv_path"

if [[ "$baseline_path" != "" ]]; then
    if [[ "$suite" == "" || "$implementation" == "" ]]; then
        echo "--suite and --implementation are required with --baseline" >&2
        exit 2
    fi
    php /workspace/tools/compare-benchmark-baseline.php \
        --baseline="$baseline_path" \
        --current="$tmp_json" \
        --suite="$suite" \
        --implementation="$implementation"
fi
