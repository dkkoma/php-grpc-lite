# Issue #5 Spanner CLI repro

Docker-based reproduction fixture copied from GitHub issue #5, with the base image adjusted from `php:8.4-cli-bookworm` to `php:8.4-cli-trixie` for this repository's current environment.

This fixture is diagnostic-only. It is not part of the standard benchmark suite.

## Build

```sh
docker build --build-arg GRPC_VARIANT=official -t spanner-repro:official .
docker build --build-arg GRPC_VARIANT=lite -t spanner-repro:lite .
```

Minimal `ExecuteStreamingSql SELECT 1` only:

```sh
docker build --build-arg GRPC_VARIANT=official --build-arg BENCH_SCRIPT=select1-bench.php -t spanner-repro:official-select1 .
docker build --build-arg GRPC_VARIANT=lite --build-arg BENCH_SCRIPT=select1-bench.php -t spanner-repro:lite-select1 .
```

## Run with service account key

```sh
SA_KEY=/path/to/sa-key.json
PROJECT=your-project
INSTANCE=your-instance
DATABASE=your-database

for tag in official lite; do
  docker run --rm \
    -v "$SA_KEY":/sa.json:ro \
    -e GOOGLE_APPLICATION_CREDENTIALS=/sa.json \
    -e GOOGLE_CLOUD_PROJECT="$PROJECT" \
    -e DB_SPANNER_INSTANCE="$INSTANCE" \
    -e DB_SPANNER_DATABASE="$DATABASE" \
    spanner-repro:$tag 200
done
```

## Run with local gcloud ADC

This is useful for local comparison, but it is not identical to the reporter's service-account JSON / JWT credential path.

```sh
PROJECT=your-project
INSTANCE=your-instance
DATABASE=your-database

for tag in official lite; do
  docker run --rm \
    -v "$HOME/.config/gcloud:/root/.config/gcloud:ro" \
    -e GOOGLE_CLOUD_PROJECT="$PROJECT" \
    -e DB_SPANNER_INSTANCE="$INSTANCE" \
    -e DB_SPANNER_DATABASE="$DATABASE" \
    spanner-repro:$tag 200
done
```
