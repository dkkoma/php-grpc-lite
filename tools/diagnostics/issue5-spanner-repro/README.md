# Issue #5 Spanner CLI repro

Docker-based reproduction fixture copied from GitHub issue #5, with the base image adjusted from `php:8.4-cli-bookworm` to `php:8.4-cli-trixie` for this repository's current environment.

This fixture is diagnostic-only. It is not part of the standard benchmark suite.

## Public GHCR images

GitHub Actions publishes public diagnostic images to GHCR:

- `ghcr.io/dkkoma/php-grpc-lite-spanner-repro:official`
- `ghcr.io/dkkoma/php-grpc-lite-spanner-repro:lite`

The image contains all diagnostic scripts. Select the script with `BENCH_SCRIPT`; the default is `cli-bench.php`.

Supported scripts:

- `cli-bench.php`: original issue #5 Spanner transaction repro
- `select1-bench.php`: minimal Spanner `ExecuteStreamingSql SELECT 1`
- `list-topics-bench.php`: Pub/Sub `ListTopics`
- `get-topic-bench.php`: Pub/Sub `GetTopic`
- `get-project-bench.php`: Resource Manager `GetProject`
- `get-secret-bench.php`: Secret Manager `GetSecret`

Initial GHCR package visibility must be set to public once after the first successful publish. Use GitHub UI: `Packages` → `php-grpc-lite-spanner-repro` → package settings → visibility.

## Build locally

```sh
docker build --build-arg GRPC_VARIANT=official -t spanner-repro:official .
docker build --build-arg GRPC_VARIANT=lite -t spanner-repro:lite .
```

To change dependency versions:

```sh
docker build \
  --build-arg GRPC_VARIANT=official \
  --build-arg GRPC_OFFICIAL_VERSION=1.58.0 \
  -t spanner-repro:official .

docker build \
  --build-arg GRPC_VARIANT=lite \
  --build-arg GRPC_LITE_VERSION=0.0.8 \
  -t spanner-repro:lite .
```

## Run with service account key

```sh
SA_KEY=/path/to/sa-key.json
PROJECT=your-project
INSTANCE=your-instance
DATABASE=your-database
IMAGE=ghcr.io/dkkoma/php-grpc-lite-spanner-repro

for variant in official lite; do
  docker run --rm \
    -v "$SA_KEY":/sa.json:ro \
    -e GOOGLE_APPLICATION_CREDENTIALS=/sa.json \
    -e GOOGLE_CLOUD_PROJECT="$PROJECT" \
    -e DB_SPANNER_INSTANCE="$INSTANCE" \
    -e DB_SPANNER_DATABASE="$DATABASE" \
    "$IMAGE:$variant" 200
done
```

Minimal `ExecuteStreamingSql SELECT 1`:

```sh
for variant in official lite; do
  docker run --rm \
    -v "$SA_KEY":/sa.json:ro \
    -e GOOGLE_APPLICATION_CREDENTIALS=/sa.json \
    -e GOOGLE_CLOUD_PROJECT="$PROJECT" \
    -e DB_SPANNER_INSTANCE="$INSTANCE" \
    -e DB_SPANNER_DATABASE="$DATABASE" \
    -e BENCH_SCRIPT=select1-bench.php \
    "$IMAGE:$variant" 200
done
```

For Secret Manager `GetSecret`, set `SECRET_ID` to an existing secret id. It defaults to `test`.

```sh
for variant in official lite; do
  docker run --rm \
    -v "$SA_KEY":/sa.json:ro \
    -e GOOGLE_APPLICATION_CREDENTIALS=/sa.json \
    -e GOOGLE_CLOUD_PROJECT="$PROJECT" \
    -e SECRET_ID=test \
    -e BENCH_SCRIPT=get-secret-bench.php \
    "$IMAGE:$variant" 200
done
```

For Pub/Sub `ListTopics`, `DB_SPANNER_INSTANCE` and `DB_SPANNER_DATABASE` are not required:

```sh
for variant in official lite; do
  docker run --rm \
    -v "$SA_KEY":/sa.json:ro \
    -e GOOGLE_APPLICATION_CREDENTIALS=/sa.json \
    -e GOOGLE_CLOUD_PROJECT="$PROJECT" \
    -e BENCH_SCRIPT=list-topics-bench.php \
    "$IMAGE:$variant" 200
done
```

For `GetTopic`, set `PUBSUB_TOPIC` to an existing topic id. It defaults to `test`.

```sh
for variant in official lite; do
  docker run --rm \
    -v "$SA_KEY":/sa.json:ro \
    -e GOOGLE_APPLICATION_CREDENTIALS=/sa.json \
    -e GOOGLE_CLOUD_PROJECT="$PROJECT" \
    -e PUBSUB_TOPIC=test \
    -e BENCH_SCRIPT=get-topic-bench.php \
    "$IMAGE:$variant" 200
done
```

## Run with local gcloud ADC

This is useful for local comparison, but it is not identical to the reporter's service-account JSON / JWT credential path.

```sh
PROJECT=your-project
INSTANCE=your-instance
DATABASE=your-database
IMAGE=ghcr.io/dkkoma/php-grpc-lite-spanner-repro

for variant in official lite; do
  docker run --rm \
    -v "$HOME/.config/gcloud:/root/.config/gcloud:ro" \
    -e GOOGLE_CLOUD_PROJECT="$PROJECT" \
    -e DB_SPANNER_INSTANCE="$INSTANCE" \
    -e DB_SPANNER_DATABASE="$DATABASE" \
    -e BENCH_SCRIPT=select1-bench.php \
    "$IMAGE:$variant" 200
done
```
