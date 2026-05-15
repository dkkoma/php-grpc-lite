#!/usr/bin/env bash
# Build official PECL ext-grpc grpc.so for version-aligned comparisons.
#
# Usage:
#   ./tools/dev/build-official-ext-grpc-so.sh <version>
#
# Output:
#   var/official-ext-grpc-so/<version>/grpc.so
set -euo pipefail

cd "$(dirname "$0")/../.."

version="${1:-}"
if [[ -z "$version" ]]; then
    echo "Usage: ./tools/dev/build-official-ext-grpc-so.sh <version>" >&2
    exit 1
fi

build_dir="var/official-ext-grpc-builds/$version"
out_dir="var/official-ext-grpc-so/$version"
out_so="$out_dir/grpc.so"

rm -rf "$build_dir"
mkdir -p "$build_dir" "$out_dir"

docker compose run --rm dev-ext-grpc sh -lc "
    set -euo pipefail
    cd /workspace/$build_dir
    pecl download grpc-$version >/dev/null
    tar xf grpc-$version.tgz
    cd grpc-$version
    phpize >/dev/null
    ./configure >/dev/null
    make -j\$(nproc) >/dev/null
    cp modules/grpc.so /workspace/$out_so
    php -n -d extension=/workspace/$out_so -r 'echo phpversion(\"grpc\"), PHP_EOL;'
"

printf 'built official ext-grpc %s -> %s\n' "$version" "$out_so"
