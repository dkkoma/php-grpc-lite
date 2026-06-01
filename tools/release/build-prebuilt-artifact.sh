#!/usr/bin/env bash
#
# Build one release grpc.so artifact tarball.
#
# Usage:
#   ./tools/release/build-prebuilt-artifact.sh <tag> <php-version> <nts|zts> <trixie> <amd64|arm64>
#
set -euo pipefail

cd "$(dirname "$0")/../.."

tag="${1:-}"
php_version="${2:-}"
thread_safety="${3:-}"
distro="${4:-}"
arch="${5:-}"

if [[ -z "$tag" || -z "$php_version" || -z "$thread_safety" || -z "$distro" || -z "$arch" ]]; then
    echo "Usage: ./tools/release/build-prebuilt-artifact.sh <tag> <php-version> <nts|zts> <trixie> <amd64|arm64>" >&2
    exit 1
fi

case "$php_version" in
    8.4|8.5) ;;
    *)
        echo "unsupported php version: $php_version" >&2
        exit 1
        ;;
esac

case "$thread_safety" in
    nts|zts) ;;
    *)
        echo "unsupported thread safety: $thread_safety" >&2
        exit 1
        ;;
esac

case "$distro" in
    trixie) ;;
    *)
        echo "unsupported distro: $distro" >&2
        exit 1
        ;;
esac

case "$arch" in
    amd64|arm64) ;;
    *)
        echo "unsupported arch: $arch" >&2
        exit 1
        ;;
esac

if [[ "$thread_safety" == "zts" ]]; then
    php_image="php:${php_version}-zts-${distro}"
else
    php_image="php:${php_version}-cli-${distro}"
fi

version="${tag#v}"
asset_base="php-grpc-lite-${version}-php${php_version}-${thread_safety}-${distro}-${arch}"
platform="linux/${arch}"
source_ref="$tag"
source_sha="$(git rev-parse HEAD 2>/dev/null || echo unknown)"

rootfs_dir="var/release-artifacts/rootfs/${asset_base}"
work_dir="var/release-artifacts/work/${asset_base}"
dist_dir="dist/release-artifacts"
asset_path="${dist_dir}/${asset_base}.tar.gz"
asset_abs="$(pwd)/$asset_path"

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$@"
    else
        shasum -a 256 "$@"
    fi
}

rm -rf "$rootfs_dir" "$work_dir"
mkdir -p "$rootfs_dir" "$work_dir" "$dist_dir"

docker buildx build \
    --platform "$platform" \
    --build-arg "PHP_IMAGE=$php_image" \
    --build-arg "VERSION=$version" \
    --build-arg "RELEASE_TAG=$tag" \
    --build-arg "PHP_VERSION=$php_version" \
    --build-arg "THREAD_SAFETY=$thread_safety" \
    --build-arg "DISTRO=$distro" \
    --build-arg "SOURCE_REF=$source_ref" \
    --build-arg "SOURCE_SHA=$source_sha" \
    --output "type=local,dest=$rootfs_dir" \
    -f docker/Dockerfile.release-artifact \
    .

test -f "$rootfs_dir/artifacts/grpc.so"
test -f "$rootfs_dir/artifacts/metadata.json"

cp -R "$rootfs_dir/artifacts" "$work_dir/artifacts"
(
    cd "$work_dir"
    sha256_file artifacts/grpc.so artifacts/metadata.json > SHA256SUMS
    tar -czf "$asset_abs" artifacts SHA256SUMS
)

printf 'built release artifact: %s\n' "$asset_path"
