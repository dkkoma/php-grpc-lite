#!/usr/bin/env bash
#
# Build research-only nghttp2 + libcurl with HTTP/2 upload instrumentation.
#
# Prerequisite:
#   git clone https://github.com/nghttp2/nghttp2.git _research/nghttp2
#   git -C _research/nghttp2 checkout v1.64.0
#   git clone https://github.com/curl/curl.git _research/curl
#   git -C _research/curl checkout curl-8_14_1
#
set -euo pipefail

cd "$(dirname "$0")/../.."

nghttp2_src="${NGHTTP2_SRC:-_research/nghttp2}"
curl_src="${CURL_SRC:-_research/curl}"
nghttp2_patch="tools/phase2/nghttp2-upload-instrumentation.patch"
curl_patch="tools/phase2/libcurl-http2-upload-instrumentation.patch"

if [[ ! -f "$nghttp2_src/lib/nghttp2_session.c" ]]; then
    cat >&2 <<EOF
Missing nghttp2 source: $nghttp2_src

Expected:
  git clone https://github.com/nghttp2/nghttp2.git _research/nghttp2
  git -C _research/nghttp2 checkout v1.64.0
EOF
    exit 2
fi

if [[ ! -f "$curl_src/lib/http2.c" ]]; then
    cat >&2 <<EOF
Missing curl source: $curl_src

Expected:
  git clone https://github.com/curl/curl.git _research/curl
  git -C _research/curl checkout curl-8_14_1
EOF
    exit 2
fi

if ! rg -q '\[NGHTTP2INST\]' "$nghttp2_src/lib/nghttp2_session.c"; then
    git -C "$nghttp2_src" apply --check "../../$nghttp2_patch"
    git -C "$nghttp2_src" apply "../../$nghttp2_patch"
fi

if ! rg -q '\[CURLINST\]' "$curl_src/lib/http2.c"; then
    git -C "$curl_src" apply --check "../../$curl_patch"
    git -C "$curl_src" apply "../../$curl_patch"
fi

docker compose run --rm dev sh -lc '
set -euo pipefail
apt-get update
apt-get install -y --no-install-recommends cmake ninja-build ca-certificates pkg-config
rm -rf /var/lib/apt/lists/*
rm -rf \
  /workspace/var/build-nghttp2-instrumented \
  /workspace/var/instrumented-nghttp2 \
  /workspace/var/build-curl-nghttp2-instrumented \
  /workspace/var/instrumented-curl-nghttp2

cmake -S /workspace/_research/nghttp2 -B /workspace/var/build-nghttp2-instrumented -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX=/workspace/var/instrumented-nghttp2 \
  -DBUILD_SHARED_LIBS=ON \
  -DBUILD_STATIC_LIBS=OFF \
  -DENABLE_LIB_ONLY=ON \
  -DENABLE_DOC=OFF \
  -DENABLE_EXAMPLES=OFF \
  -DENABLE_APP=OFF \
  -DENABLE_HPACK_TOOLS=OFF
cmake --build /workspace/var/build-nghttp2-instrumented --target install -j"$(nproc)"

PKG_CONFIG_PATH=/workspace/var/instrumented-nghttp2/lib/pkgconfig \
CMAKE_PREFIX_PATH=/workspace/var/instrumented-nghttp2 \
cmake -S /workspace/_research/curl -B /workspace/var/build-curl-nghttp2-instrumented -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX=/workspace/var/instrumented-curl-nghttp2 \
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
cmake --build /workspace/var/build-curl-nghttp2-instrumented --target install -j"$(nproc)"

LD_LIBRARY_PATH=/workspace/var/instrumented-curl-nghttp2/lib:/workspace/var/instrumented-nghttp2/lib \
  ldd /workspace/var/instrumented-curl-nghttp2/lib/libcurl.so.4 | grep /workspace/var/instrumented-nghttp2
LD_LIBRARY_PATH=/workspace/var/instrumented-curl-nghttp2/lib:/workspace/var/instrumented-nghttp2/lib \
  php -r "var_export(curl_version()[\"version\"]); echo PHP_EOL;"
'

cat <<EOF
Built instrumented nghttp2 + libcurl:
  var/instrumented-nghttp2/lib/libnghttp2.so.14
  var/instrumented-curl-nghttp2/lib/libcurl.so.4

Run with:
  LD_LIBRARY_PATH=/workspace/var/instrumented-curl-nghttp2/lib:/workspace/var/instrumented-nghttp2/lib NGHTTP2INST=1 php ...
EOF
