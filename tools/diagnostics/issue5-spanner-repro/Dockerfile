# syntax=docker/dockerfile:1.6
ARG GRPC_VARIANT=official
ARG GRPC_OFFICIAL_VERSION=1.58.0
ARG GRPC_LITE_VERSION=0.0.8

FROM php:8.4-cli-trixie AS base
RUN set -eux \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      autoconf bison cmake curl ca-certificates g++ git make pkg-config re2c \
      libssl-dev zlib1g-dev libc-ares-dev libre2-dev libnghttp2-dev unzip \
 && rm -rf /var/lib/apt/lists/*

FROM base AS grpc-official
ARG GRPC_OFFICIAL_VERSION
RUN pecl install grpc-${GRPC_OFFICIAL_VERSION} \
 && docker-php-ext-enable grpc

FROM base AS grpc-lite
ARG GRPC_LITE_VERSION
RUN curl -fsSL https://github.com/php/pie/releases/latest/download/pie.phar -o /usr/local/bin/pie \
 && chmod +x /usr/local/bin/pie \
 && pie install --auto-install-system-dependencies dkkoma/php-grpc-lite:${GRPC_LITE_VERSION}

FROM grpc-${GRPC_VARIANT} AS app
COPY --from=composer:2 /usr/bin/composer /usr/bin/composer
WORKDIR /app
COPY composer.json /app/composer.json
RUN composer install --no-progress --no-interaction --prefer-dist
COPY cli-bench.php /app/cli-bench.php
ENTRYPOINT ["php", "/app/cli-bench.php"]
CMD ["200"]
