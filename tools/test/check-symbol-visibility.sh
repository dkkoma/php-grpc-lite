#!/usr/bin/env bash
#
# Verify that the shared grpc extension only exports the PHP loader entrypoint.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

docker compose run --rm dev bash -lc '
    set -euo pipefail
    cd /workspace
    command -v nm >/dev/null || { echo "nm is not available in the dev image" >&2; exit 127; }

    check_build() {
        local label="$1"
        shift
        make clean >"/tmp/grpc-symbol-${label}-clean.log" 2>&1 || true
        rm -rf .libs modules *.lo *.o *.dep src/.libs src/*.lo src/*.o src/*.dep src/diagnostic/.libs src/diagnostic/*.lo src/diagnostic/*.o src/diagnostic/*.dep
        phpize >"/tmp/grpc-symbol-${label}-phpize.log"
        ./configure --enable-grpc "$@" >"/tmp/grpc-symbol-${label}-configure.log"
        make -j$(nproc) >"/tmp/grpc-symbol-${label}-make.log"

        php -d extension=/workspace/modules/grpc.so -r '\''exit(extension_loaded("grpc") ? 0 : 1);'\'' \
            || { echo "grpc extension failed to load from /workspace/modules/grpc.so for $label build" >&2; exit 1; }

        local symbols expected
        symbols="$(nm -D --defined-only /workspace/modules/grpc.so | awk '\''{ print $3 }'\'' | sort)"
        expected="get_module"
        if [[ "$symbols" != "$expected" ]]; then
            echo "unexpected exported symbols in modules/grpc.so for $label build:" >&2
            comm -23 <(printf "%s\n" "$symbols") <(printf "%s\n" "$expected") >&2
            echo "missing expected exported symbols for $label build:" >&2
            comm -13 <(printf "%s\n" "$symbols") <(printf "%s\n" "$expected") >&2
            exit 1
        fi
    }

    check_build production
    check_build bench --enable-grpc-bench
'
