#!/usr/bin/env bash
#
# Run the native extension lifecycle fixture under Valgrind.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

VALGRIND=1 \
    BENCH_TAG="${BENCH_TAG:-valgrind}" \
    ITERATIONS="${ITERATIONS:-5}" \
    MESSAGE_COUNT="${MESSAGE_COUNT:-20}" \
    PAYLOAD_BYTES="${PAYLOAD_BYTES:-1024}" \
    MAX_FD_DELTA="${MAX_FD_DELTA:-1}" \
    ./tools/test/check-native-lifecycle-stress.sh
