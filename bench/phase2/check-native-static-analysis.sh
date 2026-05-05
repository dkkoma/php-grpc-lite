#!/usr/bin/env bash
#
# Run static analysis for the source-built grpc extension.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

docker compose run --rm dev sh -lc '
    cd /workspace
    command -v cppcheck >/dev/null || { echo "cppcheck is not installed in the dev image" >&2; exit 127; }

    php_includes="$(php-config --includes)"
    pkg_includes="$(pkg-config --cflags libnghttp2 openssl)"

    cppcheck \
        --enable=warning,performance,portability \
        --error-exitcode=1 \
        --quiet \
        --inline-suppr \
        --suppress=missingIncludeSystem \
        --suppress=normalCheckLevelMaxBranches \
        --suppress=toomanyconfigs \
        --suppress=missingReturn:/usr/local/include/php/* \
        --suppress=autoVariables:ext/grpc/grpc.c \
        --std=c99 \
        $php_includes \
        $pkg_includes \
        ext/grpc/grpc.c

    cppcheck \
        --enable=warning,performance,portability \
        --error-exitcode=1 \
        --quiet \
        --inline-suppr \
        --suppress=missingIncludeSystem \
        --suppress=normalCheckLevelMaxBranches \
        --suppress=toomanyconfigs \
        --suppress=missingReturn:/usr/local/include/php/* \
        --suppress=autoVariables:ext/grpc/grpc.c \
        --suppress=autoVariables:ext/grpc/grpc_bench.c \
        --std=c99 \
        -DPHP_GRPC_LITE_ENABLE_BENCH=1 \
        $php_includes \
        $pkg_includes \
        ext/grpc/grpc.c
'
