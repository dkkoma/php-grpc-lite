#!/usr/bin/env bash
#
# Run static analysis for the source-built grpc extension.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

docker compose run --rm --no-deps dev sh -lc '
    cd /workspace
    command -v cppcheck >/dev/null || { echo "cppcheck is not installed in the dev image" >&2; exit 127; }

    php_includes="$(php-config --includes)"
    pkg_includes="$(pkg-config --cflags libnghttp2 openssl)"
    production_sources="grpc.c src/protocol_core.c src/status_core.c src/transport_core.c src/tls_config.c src/surface.c src/transport.c src/unary_call.c src/server_streaming_call.c src/wrapper_adapter.c"
    bench_sources="$production_sources src/diagnostic/diagnostic.c src/diagnostic/bench.c"

    cppcheck \
        --enable=warning,performance,portability \
        --error-exitcode=1 \
        --quiet \
        --inline-suppr \
        --suppress=missingIncludeSystem \
        --suppress=normalCheckLevelMaxBranches \
        --suppress=toomanyconfigs \
        --suppress=missingReturn:/usr/local/include/php/* \
        --std=c99 \
        $php_includes \
        $pkg_includes \
        $production_sources

    cppcheck \
        --enable=warning,performance,portability \
        --error-exitcode=1 \
        --quiet \
        --inline-suppr \
        --suppress=missingIncludeSystem \
        --suppress=normalCheckLevelMaxBranches \
        --suppress=toomanyconfigs \
        --suppress=missingReturn:/usr/local/include/php/* \
        --std=c99 \
        -DPHP_GRPC_LITE_ENABLE_BENCH=1 \
        $php_includes \
        $pkg_includes \
        $bench_sources
'
