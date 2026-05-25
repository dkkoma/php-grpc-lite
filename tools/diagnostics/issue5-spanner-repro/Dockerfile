# syntax=docker/dockerfile:1.6
ARG PHP_VERSION=8.4
ARG DISTRO=trixie
ARG GRPC_VARIANT=official
ARG GRPC_OFFICIAL_VERSION=1.58.0
ARG GRPC_OFFICIAL_PROFILE=pecl
ARG EXT_GRPC_ARTIFACT_ARCH=amd64
ARG EXT_GRPC_ARTIFACT_REPO=ghcr.io/dkkoma/ext-grpc-artifacts

FROM ${EXT_GRPC_ARTIFACT_REPO}:${GRPC_OFFICIAL_VERSION}-php${PHP_VERSION}-${DISTRO}-${EXT_GRPC_ARTIFACT_ARCH}-${GRPC_OFFICIAL_PROFILE} AS grpc-official-artifact

FROM php:${PHP_VERSION}-cli-${DISTRO} AS base
RUN set -eux \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      autoconf bison cmake curl ca-certificates g++ git make pkg-config re2c \
      libssl-dev zlib1g-dev libc-ares-dev libre2-dev libnghttp2-dev unzip tcpdump \
 && rm -rf /var/lib/apt/lists/*

FROM base AS grpc-official
ARG GRPC_OFFICIAL_VERSION
ARG GRPC_OFFICIAL_PROFILE
COPY --from=grpc-official-artifact /artifacts/grpc.so /tmp/official-grpc.so
COPY --from=grpc-official-artifact /artifacts/metadata.json /usr/local/share/grpc-official-artifact-metadata.json
RUN cp /tmp/official-grpc.so "$(php -r 'echo ini_get("extension_dir");')/grpc.so" \
 && rm /tmp/official-grpc.so \
 && docker-php-ext-enable grpc \
 && php -n -d extension=grpc.so -r 'echo phpversion("grpc"), PHP_EOL;' \
 && php -r 'echo file_get_contents("/usr/local/share/grpc-official-artifact-metadata.json");'

FROM base AS grpc-official-frame-trace
ARG GRPC_OFFICIAL_VERSION
COPY tools/diagnostics/issue5-spanner-repro/patches/official-grpc-1.58.0-frame-trace.patch /tmp/official-frame-trace.patch
RUN set -eux \
 && pecl download grpc-${GRPC_OFFICIAL_VERSION} \
 && tar xf grpc-${GRPC_OFFICIAL_VERSION}.tgz \
 && cd grpc-${GRPC_OFFICIAL_VERSION} \
 && patch -p1 < /tmp/official-frame-trace.patch \
 && phpize \
 && ./configure \
 && make -j$(nproc) \
 && cp modules/grpc.so "$(php -r 'echo ini_get("extension_dir");')/grpc.so" \
 && docker-php-ext-enable grpc \
 && php -n -d extension=grpc.so -r 'echo phpversion("grpc"), PHP_EOL;'

FROM base AS grpc-lite
COPY ext/grpc/config.m4 ext/grpc/*.c ext/grpc/*.h /usr/src/php/ext/grpc/
RUN docker-php-ext-install grpc

FROM grpc-${GRPC_VARIANT} AS app
COPY --from=composer:2 /usr/bin/composer /usr/bin/composer
WORKDIR /app
COPY tools/diagnostics/issue5-spanner-repro/composer.json /app/composer.json
RUN composer install --no-progress --no-interaction --prefer-dist
COPY tools/diagnostics/issue5-spanner-repro/cli-bench.php /app/cli-bench.php
COPY tools/diagnostics/issue5-spanner-repro/select1-bench.php /app/select1-bench.php
COPY tools/diagnostics/issue5-spanner-repro/select-table-marked-app-extra-header.php /app/select-table-marked-app-extra-header.php
COPY tools/diagnostics/issue5-spanner-repro/list-topics-bench.php /app/list-topics-bench.php
COPY tools/diagnostics/issue5-spanner-repro/get-topic-bench.php /app/get-topic-bench.php
COPY tools/diagnostics/issue5-spanner-repro/get-project-bench.php /app/get-project-bench.php
COPY tools/diagnostics/issue5-spanner-repro/get-secret-bench.php /app/get-secret-bench.php
COPY tools/diagnostics/issue5-spanner-repro/wire-summary.php /app/wire-summary.php
COPY tools/diagnostics/issue5-spanner-repro/entrypoint.sh /usr/local/bin/issue5-spanner-repro
COPY tools/diagnostics/issue5-spanner-repro/run-wire-diagnostic.sh /usr/local/bin/issue5-wire-diagnostic
RUN chmod +x /usr/local/bin/issue5-spanner-repro /usr/local/bin/issue5-wire-diagnostic \
 && mkdir -p /results
ENTRYPOINT ["issue5-spanner-repro"]
CMD ["200"]
