#!/usr/bin/env bash
#
# Run PHPT tests for the source-built grpc extension.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

docker compose run --rm dev bash -lc '
    set -euo pipefail
    cd /workspace/ext/grpc
    phpize >/tmp/grpc-phpize.log
    ./configure --enable-grpc >/tmp/grpc-configure.log
    make -j$(nproc) >/tmp/grpc-make.log
    cd /workspace
    test -f vendor/autoload.php || { echo "vendor/autoload.php is missing; run composer install" >&2; exit 1; }
    php -d extension=/workspace/ext/grpc/modules/grpc.so -r '\''exit(extension_loaded("grpc") ? 0 : 1);'\'' \
        || { echo "grpc extension failed to load from /workspace/ext/grpc/modules/grpc.so" >&2; exit 1; }
    php -r '\''
        foreach ([50051, 50052, 50053, 50054] as $port) {
            $connected = false;
            $lastError = "";
            for ($attempt = 1; $attempt <= 30; $attempt++) {
                $socket = @fsockopen("test-server", $port, $errno, $errstr, 1.0);
                if (is_resource($socket)) {
                    fclose($socket);
                    $connected = true;
                    break;
                }
                $lastError = $errstr;
                usleep(100000);
            }
            if (!$connected) {
                fwrite(STDERR, "test-server:$port is not reachable after retry: $lastError\n");
                exit(1);
            }
        }
    '\''
    cleanup_phpt_artifacts() {
        local test_file artifact_base
        for test_file in ext/grpc/tests/*.phpt; do
            artifact_base="${test_file%.phpt}"
            rm -f \
                "${artifact_base}.log" \
                "${artifact_base}.out" \
                "${artifact_base}.diff" \
                "${artifact_base}.exp" \
                "${artifact_base}.php" \
                "${artifact_base}.sh"
        done
    }
    cleanup_phpt_artifacts
    TEST_PHP_EXECUTABLE="$(command -v php)" \
        php /usr/local/lib/php/build/run-tests.php -q \
        -d extension=/workspace/ext/grpc/modules/grpc.so \
        /workspace/ext/grpc/tests
    cleanup_phpt_artifacts
'
