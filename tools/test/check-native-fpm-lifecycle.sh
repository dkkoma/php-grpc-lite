#!/usr/bin/env bash
#
# Verify that the native persistent channel survives PHP-FPM request
# boundaries inside a single worker process.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

timestamp="${BENCH_TAG:-$(date +%Y%m%d-%H%M%S)}"
output_dir="${TEST_OUTPUT_DIR:-var/test-results}"
requests="${REQUESTS:-10}"
mkdir -p "$output_dir"

json="$output_dir/native-fpm-lifecycle-$timestamp.json"

docker compose run --rm dev sh -lc "
    cd /workspace &&
    make -j2 >/tmp/grpc-make.log
"

docker compose up -d --build --force-recreate fpm-lifecycle >/dev/null
trap 'docker compose stop fpm-lifecycle >/dev/null 2>&1 || true' EXIT

docker compose run --rm dev sh -lc "
    set -euo pipefail
    cd /workspace
    responses=''
    for i in \$(seq 1 $requests); do
        response=\$(SCRIPT_FILENAME=/workspace/tools/benchmark/native-fpm-lifecycle-request.php \
            REQUEST_METHOD=GET \
            cgi-fcgi -bind -connect fpm-lifecycle:9000)
        body=\$(printf '%s' \"\$response\" | awk 'BEGIN{body=0} body{print} /^\\r?$/{body=1}')
        printf '%s\n' \"\$body\" | php -r '
            \$input = stream_get_contents(STDIN);
            \$data = json_decode(\$input, true, flags: JSON_THROW_ON_ERROR);
            if ((\$data[\"ok\"] ?? false) !== true) {
                fwrite(STDERR, \"FPM request failed: \" . \$input . PHP_EOL);
                exit(1);
            }
        '
        responses=\"\$responses\$body
\"
    done

    RESPONSES=\"\$responses\" php -r '
        \$lines = array_values(array_filter(explode(\"\\n\", getenv(\"RESPONSES\") ?: \"\"), static fn(\$line) => trim(\$line) !== \"\"));
        \$items = array_map(static fn(\$line) => json_decode(\$line, true, flags: JSON_THROW_ON_ERROR), \$lines);
        \$pids = array_values(array_unique(array_map(static fn(\$item) => \$item[\"pid\"], \$items)));
        \$ok = count(\$items) === $requests
            && count(\$pids) === 1
            && !in_array(false, array_map(static fn(\$item) => (bool) (\$item[\"ok\"] ?? false), \$items), true);
        \$document = [
            \"suite\" => \"native-fpm-lifecycle\",
            \"implementation\" => \"php-grpc-lite\",
            \"generated_at\" => gmdate(\"c\"),
            \"ok\" => \$ok,
            \"requests\" => count(\$items),
            \"worker_pids\" => \$pids,
            \"responses\" => \$items,
        ];
        file_put_contents(\"$json\", json_encode(\$document, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES) . \"\\n\");
        if (!\$ok) {
            fwrite(STDERR, json_encode(\$document, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES) . PHP_EOL);
            exit(1);
        }
        printf(\"FPM lifecycle OK: requests=%d pid=%s\\n\", count(\$items), implode(\",\", \$pids));
    '
"

echo "Test artifact: $json"
