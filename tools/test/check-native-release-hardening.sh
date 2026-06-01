#!/usr/bin/env bash
#
# Run the HTTP/2 transport release-hardening QA gates that are practical in the
# local Docker environment.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

tag="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"

echo "== native static analysis =="
./tools/test/check-c-static-analysis.sh

echo "== native C unit boundary tests =="
./tools/test/check-c-unit.sh

echo "== native Crash/UB check =="
FUZZ_RUNS="${RELEASE_FUZZ_RUNS:-50000}" \
    ./tools/test/check-crash-ub.sh

if [[ "${SKIP_SANITIZER:-0}" != "1" ]]; then
    echo "== native ASan/UBSan =="
    ./tools/test/check-c-sanitizer.sh

    echo "== native MSan C core =="
    ./tools/test/check-c-msan.sh

    echo "== native TSan =="
    ./tools/test/check-c-tsan.sh
fi

echo "== native lifecycle stress smoke =="
BENCH_TAG="$tag-lifecycle-smoke" \
    ITERATIONS="${SMOKE_ITERATIONS:-100}" \
    MESSAGE_COUNT="${MESSAGE_COUNT:-20}" \
    PAYLOAD_BYTES="${PAYLOAD_BYTES:-1024}" \
    MAX_FD_DELTA="${MAX_FD_DELTA:-1}" \
    ./tools/test/check-native-lifecycle-stress.sh

echo "== native lifecycle memory checker =="
BENCH_TAG="$tag-valgrind" \
    ITERATIONS="${VALGRIND_ITERATIONS:-5}" \
    MESSAGE_COUNT="${MESSAGE_COUNT:-20}" \
    PAYLOAD_BYTES="${PAYLOAD_BYTES:-1024}" \
    MAX_FD_DELTA="${MAX_FD_DELTA:-1}" \
    ./tools/test/check-c-valgrind.sh

echo "== native lifecycle long stress =="
BENCH_TAG="$tag-lifecycle-long" \
    ITERATIONS="${LONG_ITERATIONS:-1000}" \
    MESSAGE_COUNT="${MESSAGE_COUNT:-20}" \
    PAYLOAD_BYTES="${PAYLOAD_BYTES:-1024}" \
    MAX_FD_DELTA="${MAX_FD_DELTA:-1}" \
    ./tools/test/check-native-lifecycle-stress.sh

echo "== native FPM request-boundary lifecycle =="
BENCH_TAG="$tag-fpm" \
    REQUESTS="${FPM_REQUESTS:-10}" \
    ./tools/test/check-native-fpm-lifecycle.sh
