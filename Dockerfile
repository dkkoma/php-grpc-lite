FROM php:8.4-cli-trixie

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        autoconf \
        pkg-config \
        git \
        unzip \
        libcurl4-openssl-dev \
        libssl-dev \
        libnghttp2-dev \
        protobuf-compiler \
    && rm -rf /var/lib/apt/lists/*

COPY --from=composer:2 /usr/bin/composer /usr/bin/composer

RUN pecl install protobuf \
    && docker-php-ext-enable protobuf

WORKDIR /workspace

CMD ["bash"]
