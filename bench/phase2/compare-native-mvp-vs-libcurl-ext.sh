#!/usr/bin/env bash
#
# Deprecated research runner kept only as a pointer to the historical Phase 2
# decision input. Runtime transport selection has been removed; current
# benchmarks must use the nghttp2 grpc extension surface directly.
#
set -euo pipefail

cat >&2 <<'MSG'
bench/phase2/compare-native-mvp-vs-libcurl-ext.sh is archived.

The project no longer has a runtime libcurl transport or transport selection
option. Use the current benchmark runners under bench/phase2/ or read:
  docs/research/http2-transport-mvp-comparison-2026-05-03.md
MSG
exit 2
