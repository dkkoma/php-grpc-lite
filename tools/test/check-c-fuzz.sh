#!/usr/bin/env bash
#
# Run deterministic libFuzzer smoke targets for C protocol boundaries.
# This is a development/release gate, not an open-ended fuzzing campaign.
#
set -euo pipefail

cd "$(dirname "$0")/../.."

run_args=(run --rm)
if [[ "${COMPOSE_RUN_BUILD:-1}" != "0" ]]; then
    run_args+=(--build)
fi

docker compose "${run_args[@]}" \
    -e FUZZ_RUNS="${FUZZ_RUNS:-20000}" \
    -e FUZZ_MAX_LEN="${FUZZ_MAX_LEN:-256}" \
    dev-sanitizer bash -lc '
    set -euo pipefail

    cd /workspace
    build_dir=/workspace/var/fuzz
    seed_corpus_dir=/workspace/tests/fuzz/corpus/protocol_core
    corpus_dir=/workspace/var/fuzz/corpus/protocol_core
    artifact_dir=/workspace/var/fuzz/artifacts

    rm -rf "$build_dir"
    mkdir -p "$build_dir" "$artifact_dir"
    mkdir -p "$corpus_dir"
    cp -a "$seed_corpus_dir"/. "$corpus_dir"/

    export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1:abort_on_error=1:allocator_may_return_null=1:verify_asan_link_order=0}"
    export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"

    clang -D_GNU_SOURCE -std=c99 -Wall -Wextra -Wno-unused-function \
        -O1 -g -fno-omit-frame-pointer \
        -I/workspace \
        -fsanitize=fuzzer,address,undefined \
        -o "$build_dir/fuzz_protocol_core" \
        /workspace/tests/fuzz/fuzz_protocol_core.c \
        /workspace/protocol_core.c

    "$build_dir/fuzz_protocol_core" "$corpus_dir" \
        -runs="${FUZZ_RUNS:-20000}" \
        -max_len="${FUZZ_MAX_LEN:-256}" \
        -artifact_prefix="$artifact_dir/"
'
