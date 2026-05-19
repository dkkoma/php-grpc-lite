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

Pub/Sub `ListTopics` cross-check:

```sh
docker build --build-arg GRPC_VARIANT=official --build-arg BENCH_SCRIPT=list-topics-bench.php -t pubsub-repro:official-listtopics .
docker build --build-arg GRPC_VARIANT=lite --build-arg BENCH_SCRIPT=list-topics-bench.php -t pubsub-repro:lite-listtopics .
docker build --build-arg GRPC_VARIANT=official --build-arg BENCH_SCRIPT=get-topic-bench.php -t pubsub-repro:official-gettopic .
docker build --build-arg GRPC_VARIANT=lite --build-arg BENCH_SCRIPT=get-topic-bench.php -t pubsub-repro:lite-gettopic .
```

Secret Manager `GetSecret` cross-check:

```sh
docker build --build-arg GRPC_VARIANT=official --build-arg BENCH_SCRIPT=get-project-bench.php -t resourcemanager-repro:official-getproject .
docker build --build-arg GRPC_VARIANT=lite --build-arg BENCH_SCRIPT=get-project-bench.php -t resourcemanager-repro:lite-getproject .
docker build --build-arg GRPC_VARIANT=official --build-arg BENCH_SCRIPT=get-secret-bench.php -t secretmanager-repro:official-getsecret .
docker build --build-arg GRPC_VARIANT=lite --build-arg BENCH_SCRIPT=get-secret-bench.php -t secretmanager-repro:lite-getsecret .
```

Resource Manager `GetProject` only needs `GOOGLE_CLOUD_PROJECT`.

```sh
for tag in official lite; do
  docker run --rm \
    -v "$SA_KEY":/sa.json:ro \
    -e GOOGLE_APPLICATION_CREDENTIALS=/sa.json \
    -e GOOGLE_CLOUD_PROJECT="$PROJECT" \
    resourcemanager-repro:$tag-getproject 200
done
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

For Secret Manager `GetSecret`, set `SECRET_ID` to an existing secret id. It defaults to `test`.

```sh
for tag in official lite; do
  docker run --rm \
    -v "$SA_KEY":/sa.json:ro \
    -e GOOGLE_APPLICATION_CREDENTIALS=/sa.json \
    -e GOOGLE_CLOUD_PROJECT="$PROJECT" \
    -e SECRET_ID=test \
    secretmanager-repro:$tag-getsecret 200
done
```

For the Pub/Sub `ListTopics` cross-check, `DB_SPANNER_INSTANCE` and `DB_SPANNER_DATABASE` are not required:

```sh
for tag in official lite; do
  docker run --rm \
    -v "$SA_KEY":/sa.json:ro \
    -e GOOGLE_APPLICATION_CREDENTIALS=/sa.json \
    -e GOOGLE_CLOUD_PROJECT="$PROJECT" \
    pubsub-repro:$tag-listtopics 200
done
```

For `GetTopic`, set `PUBSUB_TOPIC` to an existing topic id. It defaults to `test`.

```sh
for tag in official lite; do
  docker run --rm \
    -v "$SA_KEY":/sa.json:ro \
    -e GOOGLE_APPLICATION_CREDENTIALS=/sa.json \
    -e GOOGLE_CLOUD_PROJECT="$PROJECT" \
    -e PUBSUB_TOPIC=test \
    pubsub-repro:$tag-gettopic 200
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
