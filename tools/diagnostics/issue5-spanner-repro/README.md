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

## Wire/header-size diagnostic on GCP VM

Use this mode for issue #5 header-size investigation. It is different from the simple benchmark entrypoint: it records PHP markers, grpc-lite HTTP/2 trace, tcpdump pcap, and a derived summary under `/results`.

The `lite` image is source-built from this repository branch so it contains diagnostic INI knobs such as `grpc_lite.http2_experimental_no_index_x_bench_padding`. The `official` image is PECL ext-grpc and is useful for wall-time / packet timing comparison, but it does not emit grpc-lite trace events.

Example on a GCP VM near the Spanner instance:

```sh
SA_KEY=/path/to/sa-key.json
PROJECT=vast-falcon-165704
INSTANCE=bench
DATABASE=laravel-bench-db
IMAGE=ghcr.io/dkkoma/php-grpc-lite-spanner-repro
RUN_ID=$(date -u +%Y%m%dT%H%M%SZ)

for pad in 0 500 510 520 630; do
  mkdir -p "results/lite-pad-${pad}-${RUN_ID}"
  docker run --rm \
    --network host \
    --cap-add NET_RAW --cap-add NET_ADMIN \
    -v "$SA_KEY":/sa.json:ro \
    -v "$PWD/results/lite-pad-${pad}-${RUN_ID}":/results \
    -e GOOGLE_APPLICATION_CREDENTIALS=/sa.json \
    -e GOOGLE_CLOUD_PROJECT="$PROJECT" \
    -e DB_SPANNER_INSTANCE="$INSTANCE" \
    -e DB_SPANNER_DATABASE="$DATABASE" \
    -e ITER=60 \
    -e TCPDUMP_STOP_GRACE=1 \
    -e SPANNER_GRPC_EXTRA_HEADER_BYTES="$pad" \
    -e PHP_INI_ARGS='-d grpc_lite.http2_experimental_no_index_x_bench_padding=1' \
    --entrypoint issue5-wire-diagnostic \
    "$IMAGE:lite"
done
```

For the official comparator:

```sh
mkdir -p "results/official-${RUN_ID}"
docker run --rm \
  --network host \
  --cap-add NET_RAW --cap-add NET_ADMIN \
  -v "$SA_KEY":/sa.json:ro \
  -v "$PWD/results/official-${RUN_ID}":/results \
  -e GOOGLE_APPLICATION_CREDENTIALS=/sa.json \
  -e GOOGLE_CLOUD_PROJECT="$PROJECT" \
  -e DB_SPANNER_INSTANCE="$INSTANCE" \
  -e DB_SPANNER_DATABASE="$DATABASE" \
  -e ITER=60 \
  -e TCPDUMP_STOP_GRACE=1 \
  --entrypoint issue5-wire-diagnostic \
  "$IMAGE:official"
```

Each run writes:

- `markers.log`: PHP-side operation markers and elapsed time
- `php.err`: PHP stderr
- `trace.jsonl`: grpc-lite wire trace; empty for official ext-grpc
- `tcpdump.pcap`: packet capture for `tcp port 443`
- `tcpdump.log`: tcpdump stderr/statistics
- `summary.txt`: derived grpc-lite stream summary from trace and markers

Use `--network host` on the VM. Without host networking, tcpdump can see packets through the filter but may not persist a complete pcap from the container network namespace. `TCPDUMP_STOP_GRACE` only waits after the PHP marker/trace measurement has finished so tcpdump can flush captured packets; it is not part of the measured RPC interval.

Interpretation target:

- `header_payload_len_unique` from `summary.txt` shows grpc-lite request HEADERS payload sizes.
- `headers_to_first_in_us_*` estimates client-observed outbound HEADERS to first inbound HTTP/2 frame latency.
- `tcpdump.pcap` is used to verify outbound TLS packet size and inbound packet timing for the same run.
