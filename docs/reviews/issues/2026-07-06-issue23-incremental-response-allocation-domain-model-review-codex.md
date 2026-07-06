# issue23 incremental response allocation domain model review 2026-07-06

## Scope

- `src/transport.c`
- `poc/test-server/main.go`
- `tests/phpt/022-error-and-http-validation.phpt`
- `docs/issues/open/2026-07-06-github-issue-23-incremental-response-allocation.md`

## Reviewer Role

- HTTP/2 / gRPC domain model reviewer (Codex)

## Review Prompt Summary

- GitHub issue #23 の uncommitted changes について、gRPC response direct decode の incremental payload allocation、declared-large-truncated fixture、PHPT memory/status assertions、issue doc が、repository の HTTP/2 / gRPC domain model と責務境界に合っているかを確認した。

## Issues

- none

## Review Notes

- `grpc_protocol_process_response_data_direct()` は、gRPC 5B frame の declared payload length を message completion target として保持しつつ、`zend_string` の確保量だけを実受信済みpayload bytesに比例させている。これは gRPC frame parser / call-local response state の責務内に収まり、HTTP/2 connection lifecycle や channel identity へ状態を漏らしていない。
- truncated body は既存の unary / server streaming completion checks で `malformed_response_frame` に分類される。RST_STREAM mid-message の taxonomy は維持され、stream-local failure と connection failure の境界も変更されていない。
- `declared-large-truncated` fixture は raw h2c validation fixtureの範囲に閉じており、production transport責務へ診断専用概念を持ち込んでいない。
- PHPT は unary / server streaming の status taxonomy と peak memory bound を同じ fixtureで固定しており、issue doc の判断ログにある「declared length は completion target、allocation は受信済み量」というモデルと整合している。

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none
