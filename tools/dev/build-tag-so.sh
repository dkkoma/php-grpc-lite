#!/usr/bin/env bash
# Build grpc.so from a git tag without modifying the current working tree.
#
# Usage:
#   ./tools/dev/build-tag-so.sh <tag>
#
# Output:
#   var/tag-so/<tag>/grpc.so
set -euo pipefail

cd "$(dirname "$0")/../.."

tag="${1:-}"
if [[ -z "$tag" ]]; then
    echo "Usage: ./tools/dev/build-tag-so.sh <tag>" >&2
    exit 1
fi

git rev-parse --verify --quiet "$tag^{commit}" >/dev/null || {
    echo "unknown git tag or commit: $tag" >&2
    exit 1
}

safe_tag="${tag//\//_}"
build_dir="var/tag-builds/$safe_tag"
out_dir="var/tag-so/$safe_tag"
out_so="$out_dir/grpc.so"

rm -rf "$build_dir"
mkdir -p "$build_dir" "$out_dir"

git archive "$tag" | tar -x -C "$build_dir"

docker compose run --rm dev sh -lc "
    set -euo pipefail
    cd /workspace/$build_dir
    if [[ ! -f config.m4 && -f ext/grpc/config.m4 ]]; then
        cd ext/grpc
    fi
    phpize >/dev/null
    ./configure >/dev/null
    make -j\$(nproc) >/dev/null
    cp modules/grpc.so /workspace/$out_so
    php -d extension=/workspace/$out_so -r 'echo phpversion(\"grpc\"), PHP_EOL;'
"

printf 'built %s -> %s\n' "$tag" "$out_so"
