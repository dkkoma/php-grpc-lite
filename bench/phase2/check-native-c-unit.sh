#!/usr/bin/env bash
#
# Run focused C unit tests for pure ext/grpc protocol helpers.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

docker compose run --rm dev bash -lc '
    set -euo pipefail
    cd /workspace
    build_dir=/workspace/var/coverage/c-unit
    rm -rf "$build_dir"
    mkdir -p "$build_dir"
    cc -std=c99 -Wall -Wextra -O0 -g \
        -o "$build_dir/test_protocol_core" \
        /workspace/ext/grpc/tests/unit/test_protocol_core.c
    "$build_dir/test_protocol_core"
'
