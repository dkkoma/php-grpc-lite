#!/usr/bin/env bash
# Build official PECL ext-grpc grpc.so for exceptional local custom comparisons.
#
# Normal benchmark/diagnostic images must use ghcr.io/dkkoma/ext-grpc-artifacts.
# Keep this helper only for cases where a source build or local patch is the
# object of investigation.
#
# Usage:
#   ./tools/dev/build-official-ext-grpc-so.sh <version> [profile]
#
# Profiles:
#   default   phpize/configure default flags
#   optimized O3 + LTO + fno-semantic-interposition
#
# Output:
#   var/official-ext-grpc-so/<version>-<profile>/grpc.so
set -euo pipefail

cd "$(dirname "$0")/../.."

version="${1:-}"
profile="${2:-default}"
if [[ -z "$version" ]]; then
    echo "Usage: ./tools/dev/build-official-ext-grpc-so.sh <version> [default|optimized]" >&2
    exit 1
fi
case "$profile" in
    default|optimized)
        ;;
    *)
        echo "unknown profile: $profile" >&2
        echo "Usage: ./tools/dev/build-official-ext-grpc-so.sh <version> [default|optimized]" >&2
        exit 1
        ;;
esac

build_key="$version-$profile"
build_dir="var/official-ext-grpc-builds/$build_key"
out_dir="var/official-ext-grpc-so/$build_key"
out_so="$out_dir/grpc.so"

if [[ "$profile" == "optimized" ]]; then
    cflags="-O3 -flto -fno-semantic-interposition"
    cxxflags="-O3 -flto -fno-semantic-interposition"
    ldflags="-flto"
else
    cflags=""
    cxxflags=""
    ldflags=""
fi

rm -rf "$build_dir"
mkdir -p "$build_dir" "$out_dir"

docker compose run --rm dev-ext-grpc sh -lc "
    set -euo pipefail
    cd /workspace/$build_dir
    pecl download grpc-$version >/dev/null
    tar xf grpc-$version.tgz
    cd grpc-$version
    phpize >/dev/null
    CFLAGS='$cflags' CXXFLAGS='$cxxflags' LDFLAGS='$ldflags' ./configure >/dev/null
    make -j\$(nproc) >/dev/null
    cp modules/grpc.so /workspace/$out_so
    php -n -d extension=/workspace/$out_so -r 'echo phpversion(\"grpc\"), PHP_EOL;'
"

printf 'built official ext-grpc %s profile=%s -> %s\n' "$version" "$profile" "$out_so"
