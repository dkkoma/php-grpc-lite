#!/usr/bin/env bash
#
# 1 RPC あたりの wire I/O 回数 (wire.tls_read / wire.tls_write / wire.socket_read /
# wire.socket_write) を GRPC_LITE_TRACE_FILE で計測する。syscall 削減系 issue の
# before/after 記録用。
#
# Usage: ./tools/benchmark/trace-io-probe.sh [calls] [payload_bytes]
#
set -euo pipefail

cd "$(dirname "$0")/../.."

calls="${1:-10}"
payload="${2:-1048576}"

docker compose run --rm \
    -e BENCH_OTEL_EXPORTER=otlp-http \
    -e BENCH_OTEL_EXPORTER_OTLP_ENDPOINT=http://otelop:4318/v1/traces \
    -e BENCH_OTEL_RUN_ID=trace-io-probe \
    -e NO_PROXY=test-server,otelop,localhost,127.0.0.1 \
    dev bash -lc "
set -euo pipefail
cd /workspace
run_case() {
    local label=\$1 suite=\$2 direction_arg=\$3
    local trace=/tmp/trace-\$label.jsonl
    rm -f \"\$trace\"
    GRPC_LITE_TRACE_FILE=\"\$trace\" php -d extension=/workspace/modules/grpc.so tools/benchmark/payload-unary.php \
        --suite=\"\$suite\" --implementation=php-grpc-lite --autoload=vendor/autoload.php \
        \"\$direction_arg\" --max-calls=$calls --duration=600 --warmup-calls=0 >/dev/null
    echo \"--- \$label (per \$((${calls})) calls, payload=${payload}) ---\"
    for ev in wire.tls_read wire.tls_write wire.socket_read wire.socket_write; do
        local n
        n=\$(grep -c \"\\\"event\\\":\\\"\$ev\\\"\" \"\$trace\" || true)
        echo \"\$ev: \$n\"
    done
}
run_case plain-recv payload-unary --payload-sizes=${payload}
run_case tls-recv tls-payload-unary --payload-sizes=${payload}
run_case plain-send upload-unary --request-payload-sizes=${payload}
run_case tls-send tls-upload-unary --request-payload-sizes=${payload}
"
