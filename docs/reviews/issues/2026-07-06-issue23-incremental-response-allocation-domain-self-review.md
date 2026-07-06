# issue #23 incremental response allocation domain review 2026-07-06

## Scope

- `src/transport.c`
- `poc/test-server/main.go`
- `tests/phpt/022-error-and-http-validation.phpt`
- `docs/issues/open/2026-07-06-github-issue-23-incremental-response-allocation.md`

## Reviewer Role

- HTTP/2/gRPC domain model self-review

## Review Prompt Summary

- response direct decodeのpayload allocation変更について、gRPC frame parser、call state、stream-local failure taxonomy、production/test boundary、unary/server streaming共通経路の責務分離を確認する。

## Issues

### REVIEW-20260706-001: findingsなし

- Severity: `Design Decision`
- Status: `Accepted`
- Reviewer role: `HTTP/2/gRPC domain model self-review`
- Finding: `Blocker / High / Medium / Low findingsなし。`
- Evidence: `src/transport.c:grpc_protocol_process_response_data_direct`, `tests/phpt/022-error-and-http-validation.phpt`
- Expected model: `gRPC 5B frame parserはdeclared payload lengthをmessage completion targetとして保持しつつ、buffer allocationは実際に受信済みのpayload bytesに比例する。truncated messageはstream-local malformed responseとして扱い、HTTP/2 connection lifecycleへ昇格しない。`
- Why it matters: `length-prefixのみで巨大allocationへ進むと、gRPC message size semanticsとprocess memory ownershipが混ざり、悪意あるstream-local inputがprocess-wide availabilityに波及する。`
- Recommended fix: `現行差分のまま、validation orderを維持し、allocation/reallocのみを受信済みbytesに比例させる。`
- Fix summary: `prefix parse後の全量 `zend_string_alloc(response_payload_len)` をやめ、初期確保は現在chunk内のpayload bytes以下にした。copy直前に必要な分だけ `zend_string_realloc()` する。`ZSTR_MAX_LEN` を超える宣言長はunrepresentableなmessageとしてmessage-too-large taxonomyへ寄せる。`
- Fix commit: `pending`
- Verification: `./tools/test/check-phpt.sh tests/phpt/022-error-and-http-validation.phpt tests/phpt/025-resource-limits.phpt` PASS; `./tools/test/check-c-unit.sh` PASS; `./tools/test/check-c-static-analysis.sh` PASS
- Notes: `raw h2c fixtureの `declared-large-truncated` はtest boundary内に閉じ、production transportにdiagnostic knobは追加していない。`

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `1 accepted`
