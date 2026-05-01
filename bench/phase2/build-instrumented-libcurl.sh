#!/usr/bin/env bash
#
# Build a research-only libcurl with HTTP/2 upload path instrumentation.
#
# Prerequisite:
#   git clone https://github.com/curl/curl.git _research/curl
#   git -C _research/curl checkout curl-8_14_1
#
# Output:
#   var/instrumented-curl/lib/libcurl.so.4
#
set -euo pipefail

cd "$(dirname "$0")/../.."

curl_src="${CURL_SRC:-_research/curl}"
patch_path="tools/phase2/libcurl-http2-upload-instrumentation.patch"

if [[ ! -f "$curl_src/lib/http2.c" ]]; then
    cat >&2 <<EOF
Missing curl source: $curl_src

Expected:
  git clone https://github.com/curl/curl.git _research/curl
  git -C _research/curl checkout curl-8_14_1
EOF
    exit 2
fi

if ! rg -q '\[CURLINST\]' "$curl_src/lib/http2.c"; then
    git -C "$curl_src" apply --check "../../$patch_path"
    git -C "$curl_src" apply "../../$patch_path"
fi

docker compose run --rm dev sh -lc '
set -euo pipefail
apt-get update
apt-get install -y --no-install-recommends cmake ninja-build ca-certificates
rm -rf /var/lib/apt/lists/*
rm -rf /workspace/var/build-curl-instrumented /workspace/var/instrumented-curl
cmake -S /workspace/_research/curl -B /workspace/var/build-curl-instrumented -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX=/workspace/var/instrumented-curl \
  -DBUILD_SHARED_LIBS=ON \
  -DBUILD_STATIC_LIBS=OFF \
  -DBUILD_CURL_EXE=OFF \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_LIBCURL_DOCS=OFF \
  -DBUILD_MISC_DOCS=OFF \
  -DENABLE_CURL_MANUAL=OFF \
  -DCURL_USE_OPENSSL=ON \
  -DUSE_NGHTTP2=ON \
  -DCURL_ZLIB=ON \
  -DCURL_BROTLI=OFF \
  -DCURL_ZSTD=OFF
cmake --build /workspace/var/build-curl-instrumented --target install -j"$(nproc)"
LD_LIBRARY_PATH=/workspace/var/instrumented-curl/lib php -r "var_export(curl_version()[\"version\"]); echo PHP_EOL;"
'

cat <<EOF
Built instrumented libcurl:
  var/instrumented-curl/lib/libcurl.so.4

Run with:
  LD_LIBRARY_PATH=/workspace/var/instrumented-curl/lib php ...
EOF
