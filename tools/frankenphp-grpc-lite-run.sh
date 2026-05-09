#!/usr/bin/env bash
#
# Run a PHP script through FrankenPHP's php-cli with this repository's grpc.so
# loaded from the source-built extension.
set -euo pipefail

cd "$(dirname "$0")/.."

if ! command -v frankenphp >/dev/null 2>&1; then
    echo "frankenphp binary is required in PATH" >&2
    exit 127
fi

if [[ ! -f ext/grpc/modules/grpc.so ]]; then
    (
        cd ext/grpc
        phpize >/tmp/php-grpc-lite-franken-phpize.log
        ./configure --enable-grpc >/tmp/php-grpc-lite-franken-configure.log
        make -j"$(nproc)" >/tmp/php-grpc-lite-franken-make.log
    )
fi

ini_dir="$(mktemp -d)"
trap 'rm -rf "$ini_dir"' EXIT
printf "extension=%s/ext/grpc/modules/grpc.so\n" "$PWD" > "$ini_dir/20-grpc.ini"

PHP_INI_SCAN_DIR="${PHP_INI_SCAN_DIR:-}:$ini_dir" frankenphp php-cli "$@"
