ARG PHP_VERSION=8.4
ARG DISTRO=trixie
ARG GRPC_OFFICIAL_VERSION=1.58.0
ARG GRPC_OFFICIAL_PROFILE=optimized
ARG EXT_GRPC_ARTIFACT_REPO=ghcr.io/dkkoma/ext-grpc-artifacts
ARG TARGETARCH

FROM ${EXT_GRPC_ARTIFACT_REPO}:${GRPC_OFFICIAL_VERSION}-php${PHP_VERSION}-${DISTRO}-${TARGETARCH}-${GRPC_OFFICIAL_PROFILE} AS official-grpc-artifact

FROM php:${PHP_VERSION}-fpm-${DISTRO} AS app-base

ARG FPM_MAX_CHILDREN=16

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        autoconf \
        pkg-config \
        git \
        unzip \
        libssl-dev \
        libnghttp2-dev \
        protobuf-compiler \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

COPY --from=composer:2 /usr/bin/composer /usr/bin/composer

RUN pecl install protobuf \
    && docker-php-ext-enable protobuf

WORKDIR /workspace
COPY . /workspace

RUN composer --working-dir=/workspace/tools/benchmark/laravel-spanner-app \
        install --no-interaction --prefer-dist --no-dev --optimize-autoloader \
    && mkdir -p \
        /workspace/tools/benchmark/laravel-spanner-app/storage/framework/spanner \
        /workspace/tools/benchmark/laravel-spanner-app/storage/framework/views \
        /workspace/tools/benchmark/laravel-spanner-app/storage/logs \
        /workspace/tools/benchmark/laravel-spanner-app/bootstrap/cache \
    && chown -R www-data:www-data \
        /workspace/tools/benchmark/laravel-spanner-app/storage \
        /workspace/tools/benchmark/laravel-spanner-app/bootstrap/cache \
    && { \
        echo '[www]'; \
        echo 'pm = static'; \
        echo "pm.max_children = ${FPM_MAX_CHILDREN}"; \
        echo 'pm.max_requests = 0'; \
        echo 'clear_env = no'; \
        echo 'catch_workers_output = yes'; \
    } > /usr/local/etc/php-fpm.d/zz-grpc-lifecycle.conf

ENV HOME=/var/www \
    BENCH_TRANSPORT_IMPL=grpc-lite \
    SPANNER_EMULATOR_HOST= \
    DB_SPANNER_PROJECT_ID=vast-falcon-165704 \
    DB_SPANNER_INSTANCE_ID=bench \
    DB_SPANNER_DATABASE_ID=laravel-bench-db \
    LARAVEL_SPANNER_MIN_SESSIONS=16

FROM app-base AS lite

RUN cd /workspace/ext/grpc \
    && phpize \
    && ./configure --enable-grpc \
    && make -j"$(nproc)" \
    && make install \
    && echo 'extension=grpc.so' > /usr/local/etc/php/conf.d/zz-grpc-lite.ini \
    && php -m | grep -x grpc \
    && php -r 'echo phpversion("grpc"), PHP_EOL;'

CMD ["sh", "-lc", "su -s /bin/sh www-data -c 'php /workspace/tools/benchmark/laravel-spanner-app/bin/startup-warmup.php' && exec php-fpm -F"]

FROM app-base AS official

ARG GRPC_OFFICIAL_VERSION=1.58.0
ARG GRPC_OFFICIAL_PROFILE=optimized

COPY --from=official-grpc-artifact /artifacts/grpc.so /tmp/official-grpc.so
COPY --from=official-grpc-artifact /artifacts/metadata.json /usr/local/share/grpc-official-artifact-metadata.json

RUN cp /tmp/official-grpc.so "$(php-config --extension-dir)/grpc.so" \
    && rm /tmp/official-grpc.so \
    && echo 'extension=grpc.so' > /usr/local/etc/php/conf.d/zz-official-grpc.ini \
    && php -m | grep -x grpc \
    && php -r 'echo phpversion("grpc"), PHP_EOL;' \
    && php -r 'echo file_get_contents("/usr/local/share/grpc-official-artifact-metadata.json");'

ENV BENCH_TRANSPORT_IMPL=ext-grpc

CMD ["sh", "-lc", "su -s /bin/sh www-data -c 'php /workspace/tools/benchmark/laravel-spanner-app/bin/startup-warmup.php' && exec php-fpm -F"]

FROM nginx:1.27-alpine AS nginx

COPY tools/benchmark/nginx/laravel-spanner.conf.template /etc/nginx/templates/default.conf.template

ENV FPM_UPSTREAM=laravel-fpm \
    SPANNER_EMULATOR_HOST= \
    DB_SPANNER_PROJECT_ID=vast-falcon-165704 \
    DB_SPANNER_INSTANCE_ID=bench \
    DB_SPANNER_DATABASE_ID=laravel-bench-db

FROM golang:1.25-trixie AS loadgen-build

RUN go install github.com/rakyll/hey@latest

FROM debian:trixie-slim AS loadgen

COPY --from=loadgen-build /go/bin/hey /usr/local/bin/hey
ENTRYPOINT ["hey"]
