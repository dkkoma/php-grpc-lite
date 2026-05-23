#!/bin/sh
set -eu

script="${BENCH_SCRIPT:-cli-bench.php}"
case "$script" in
  cli-bench.php|select1-bench.php|list-topics-bench.php|get-topic-bench.php|get-project-bench.php|get-secret-bench.php)
    ;;
  *)
    echo "Unsupported BENCH_SCRIPT: $script" >&2
    exit 64
    ;;
esac

exec php "/app/$script" "$@"
