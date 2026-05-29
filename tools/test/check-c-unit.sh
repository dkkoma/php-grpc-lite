#!/usr/bin/env bash
#
# Run focused C unit tests for pure grpc extension protocol helpers.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

docker compose run --rm dev bash -lc '
    set -euo pipefail
    cd /workspace
    build_dir=/workspace/var/coverage/c-unit
    rm -rf "$build_dir"
    mkdir -p "$build_dir"
    php_includes="$(php-config --includes)"
    pkg_includes="$(pkg-config --cflags libnghttp2 openssl)"
    for test_source in /workspace/tests/unit/test_*.c; do
        test_name="$(basename "$test_source" .c)"
        cc -D_GNU_SOURCE -std=c99 -Wall -Wextra -Wno-unused-function -Wno-unused-variable -O0 -g \
            $php_includes $pkg_includes \
            -o "$build_dir/$test_name" \
            "$test_source"
        "$build_dir/$test_name"
    done
'
