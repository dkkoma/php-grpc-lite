#!/usr/bin/env bash
#
# Compare PHP call-path concurrency between NTS multi-process and ZTS threads.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

workers="${ZTS_PARALLEL_WORKERS:-1,2,8}"
calls="${ZTS_PARALLEL_CALLS:-20}"
warmup_calls="${ZTS_PARALLEL_WARMUP_CALLS:-2}"
server_delay_ms="${ZTS_PARALLEL_SERVER_DELAY_MS:-10}"
stream_messages="${ZTS_PARALLEL_STREAM_MESSAGES:-2}"
payload_bytes="${ZTS_PARALLEL_PAYLOAD_BYTES:-100}"

build_extension() {
    local service="$1"
    local label="$2"

    echo "== build extension for $label =="
    docker compose run --build --rm "$service" bash -lc '
        set -euo pipefail
        cd /workspace
        make clean >/tmp/grpc-zts-parallel-clean.log 2>&1 || true
        rm -rf .libs modules *.lo *.o *.dep
        phpize >/tmp/grpc-zts-parallel-phpize.log
        ./configure --enable-grpc >/tmp/grpc-zts-parallel-configure.log
        make -j$(nproc) >/tmp/grpc-zts-parallel-make.log
        cd /workspace
        php -d extension=/workspace/modules/grpc.so -r '\''exit(extension_loaded("grpc") ? 0 : 1);'\''
    '
}

build_extension dev nts

echo "== NTS process concurrency =="
docker compose run --rm dev php -d extension=/workspace/modules/grpc.so \
    tools/benchmark/zts-parallel-call-path.php \
    --mode=process \
    --workers="$workers" \
    --calls="$calls" \
    --warmup-calls="$warmup_calls" \
    --server-delay-ms="$server_delay_ms" \
    --stream-messages="$stream_messages" \
    --payload-bytes="$payload_bytes"

build_extension dev-zts zts

echo "== ZTS thread concurrency =="
docker compose run --rm dev-zts php -d extension=/workspace/modules/grpc.so \
    tools/benchmark/zts-parallel-call-path.php \
    --mode=thread \
    --workers="$workers" \
    --calls="$calls" \
    --warmup-calls="$warmup_calls" \
    --server-delay-ms="$server_delay_ms" \
    --stream-messages="$stream_messages" \
    --payload-bytes="$payload_bytes"
