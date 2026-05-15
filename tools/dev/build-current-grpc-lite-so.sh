#!/usr/bin/env bash
# Build grpc-lite grpc.so from the current working tree with a selectable profile.
#
# Usage:
#   ./tools/dev/build-current-grpc-lite-so.sh [profile]
#
# Profiles:
#   default   phpize/configure default flags
#   optimized O3 + LTO + fno-semantic-interposition
#
# Output:
#   var/current-grpc-lite-so/<profile>/grpc.so
set -euo pipefail

cd "$(dirname "$0")/../.."

profile="${1:-default}"
case "$profile" in
    default|optimized)
        ;;
    *)
        echo "Usage: ./tools/dev/build-current-grpc-lite-so.sh [default|optimized]" >&2
        exit 1
        ;;
esac

out_dir="var/current-grpc-lite-so/$profile"
out_so="$out_dir/grpc.so"

if [[ "$profile" == "optimized" ]]; then
    cflags="-O3 -flto -fno-semantic-interposition"
    ldflags="-flto"
else
    cflags=""
    ldflags=""
fi

mkdir -p "$out_dir"

docker compose run --rm dev sh -lc "
    set -euo pipefail
    cd /workspace/ext/grpc
    phpize >/dev/null
    CFLAGS='$cflags' LDFLAGS='$ldflags' ./configure --enable-grpc >/dev/null
    make -j\$(nproc) >/dev/null
    cp modules/grpc.so /workspace/$out_so
    php -n -d extension=/workspace/$out_so -r 'echo phpversion(\"grpc\"), PHP_EOL;'
"

printf 'built grpc-lite current profile=%s -> %s\n' "$profile" "$out_so"
