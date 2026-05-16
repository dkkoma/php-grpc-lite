---
Status: Closed
Owner: Codex
Created: 2026-05-16
Scope: ext/grpc/bridge.c, ext/grpc/tests/010-unary.phpt
Role: PHP extension zval ownership review
---

# StartBatch metadata COW review

## Findings

- Blocker: none
- High: none
- Medium: none
- Low: initial test missed raw `Grpc\Call::startBatch()` event metadata boundary; fixed by adding direct raw startBatch mutation assertion in `ext/grpc/tests/010-unary.phpt`.

## Checks

- `add_property_zval(event, "metadata", metadata)` increments/ref-shares the metadata zval; userland writes trigger PHP array COW and do not mutate the internal call metadata array.
- Public result shape remains `stdClass->metadata` array.
- Empty metadata fallback still returns an array.

## Verification

- `make test TESTS="tests/010-unary.phpt tests/011-server-streaming.phpt tests/023-metadata-and-call-credentials.phpt"`: PASS
- `./tools/test/check-phpt.sh`: PASS, 15/15
- `./tools/test/check-c-static-analysis.sh`: PASS
