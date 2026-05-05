#!/usr/bin/env bash
#
# Run the HTTP/2 transport release-hardening QA gates that are practical in the
# local Docker environment.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

tag="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"

echo "== native static analysis =="
./bench/phase2/check-native-static-analysis.sh

echo "== native lifecycle stress smoke =="
BENCH_TAG="$tag-lifecycle-smoke" \
    ITERATIONS="${SMOKE_ITERATIONS:-100}" \
    MESSAGE_COUNT="${MESSAGE_COUNT:-20}" \
    PAYLOAD_BYTES="${PAYLOAD_BYTES:-1024}" \
    MAX_FD_DELTA="${MAX_FD_DELTA:-1}" \
    ./bench/phase2/check-native-lifecycle-stress.sh

echo "== native lifecycle memory checker =="
VALGRIND=1 \
    BENCH_TAG="$tag-valgrind" \
    ITERATIONS="${VALGRIND_ITERATIONS:-5}" \
    MESSAGE_COUNT="${MESSAGE_COUNT:-20}" \
    PAYLOAD_BYTES="${PAYLOAD_BYTES:-1024}" \
    MAX_FD_DELTA="${MAX_FD_DELTA:-1}" \
    ./bench/phase2/check-native-lifecycle-stress.sh

echo "== native lifecycle long stress =="
BENCH_TAG="$tag-lifecycle-long" \
    ITERATIONS="${LONG_ITERATIONS:-1000}" \
    MESSAGE_COUNT="${MESSAGE_COUNT:-20}" \
    PAYLOAD_BYTES="${PAYLOAD_BYTES:-1024}" \
    MAX_FD_DELTA="${MAX_FD_DELTA:-1}" \
    ./bench/phase2/check-native-lifecycle-stress.sh

echo "== native FPM request-boundary lifecycle =="
BENCH_TAG="$tag-fpm" \
    REQUESTS="${FPM_REQUESTS:-10}" \
    ./bench/phase2/check-native-fpm-lifecycle.sh
