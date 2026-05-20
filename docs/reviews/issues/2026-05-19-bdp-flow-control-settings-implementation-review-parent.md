# BDP flow-control SETTINGS implementation review 2026-05-19

## Scope

- `ext/grpc/internal.h`
- `ext/grpc/main.c`
- `ext/grpc/transport.c`
- `ext/grpc/tests/002-ini.phpt`
- `ext/grpc/tests/029-trace-file.phpt`
- `docs/issues/open/2026-05-19-bdp-flow-control-settings.md`

## Reviewer Role

- gRPC/HTTP2 transport implementation reviewer

## Review Prompt Summary

- active PING ACK後にopt-inでHTTP/2 SETTINGS updateをqueueする実装が、HTTP/2 connection-level control stateとして安全にモデル化されているか確認した。

## Issues

### REVIEW-20260519-001: findings after implementation review

- Severity: `Design Decision`
- Status: `Accepted`
- Reviewer role: `gRPC/HTTP2 transport implementation reviewer`
- Finding: 実装は完全なBDP estimatorではなく、client-origin active PING ACK後に静的target SETTINGSを単調増加で送るopt-in実験機能である。
- Evidence: `ext/grpc/transport.c:maybe_submit_active_bdp_settings_update`, `docs/issues/open/2026-05-19-bdp-flow-control-settings.md`
- Expected model: この機能は `WINDOW_UPDATE` やBDP estimatorではなく、HTTP/2 connection-level SETTINGS update実験として扱う。
- Why it matters: 計測結果をBDP estimatorの効果として誤読しないため。
- Recommended fix: 実装・issue・検証名でBDP estimatorではないことを維持する。
- Fix summary: issueに静的target SETTINGS実験であること、connection WINDOW_UPDATEは非スコープであること、INI matrix、単調増加、同一target再送抑制を明記済み。
- Fix commit: `pending`
- Verification: `manual diff review; ./tools/test/check-phpt.sh PASS; ./tools/test/check-c-unit.sh PASS; ./tools/test/check-c-static-analysis.sh PASS; real Cloud Spanner SELECT 1 measured no improvement from SETTINGS update`
- Notes:

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `1 accepted`
