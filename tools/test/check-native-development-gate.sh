#!/usr/bin/env bash
#
# Fast local development gate for changes touching the grpc C extension or PHP
# wrapper behavior. Release-only stress and Valgrind gates stay in
# check-native-release-hardening.sh.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

echo "== C static analysis =="
./tools/test/check-c-static-analysis.sh

echo "== C unit boundary tests =="
./tools/test/check-c-unit.sh

echo "== PHPT integration tests =="
./tools/test/check-phpt.sh

echo "== C protocol fuzz smoke =="
FUZZ_RUNS="${FUZZ_RUNS:-5000}" \
    ./tools/test/check-c-fuzz.sh
