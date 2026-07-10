# PR #28 status taxonomy test review 2026-07-10

## Scope

- `tests/unit/test_status_core.c`
- `tests/phpt/022-error-and-http-validation.phpt`
- `tests/Integration/CompressionTest.php`
- `src/status_core.c`
- `src/transport.c`
- `poc/test-server/main.go`
- `docs/SPEC.md`

## Reviewer Role

- test/compatibility reviewer

## Review Prompt Summary

- PR #28 (`cfb87b3`) の status taxonomy 変更について、対象テストが unary / server streaming と details 文字列を十分に固定しているかを確認した。

## Issues

### REVIEW-20260710-001: missing trailers の PHP-visible status/details が未固定

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `test/compatibility reviewer`
- Finding: `:status 200` で stream が clean END_STREAM したが `grpc-status` trailers が無いケースは、`tests/unit/test_status_core.c` で `GRPC_STATUS_INTERNAL` への分類だけを検証している。一方、PR で追加された user-visible details 文字列 `"server closed the stream without sending trailers"` は `UnaryCall::wait()` / `ServerStreamingCall::getStatus()` 経由で検証されていない。`tests/phpt/022-error-and-http-validation.phpt` と `tests/Integration/CompressionTest.php` は compressed flag / unsupported `grpc-encoding` の `INTERNAL` と details を unary / server streaming で固定しているが、missing trailers の wire-level fixture と PHP surface assertion は無い。
- Evidence: `tests/unit/test_status_core.c:95` は status code のみ、`tests/phpt/022-error-and-http-validation.phpt:48` と `tests/phpt/022-error-and-http-validation.phpt:116` は圧縮系のみ、`tests/Integration/CompressionTest.php:42` と `tests/Integration/CompressionTest.php:71` も圧縮系のみ。新しい details は `src/transport.c:2228` で生成されるが、`poc/test-server/main.go` の `50054` fixture には `content-type: application/grpc` + DATA/END_STREAM + `grpc-status` なし、または headers-only END_STREAM + `grpc-status` なしを返す knob が無い。
- Expected model: `docs/SPEC.md` では status は HTTP/2 trailers の `grpc-status` / `grpc-message` に由来し、Status object の `details` は PHP API 互換 surface の一部である。trailers 欠落は protocol violation として `STATUS_INTERNAL` に合成されるなら、unary と server streaming の PHP-visible status object で code と details の両方が固定されるべき。
- Why it matters: C unit だけでは `grpc_lite_status_details_from_call()`、metadata/trailer classification、unary result assembly、server streaming final status assembly の結合を通らない。将来、missing trailers が `INTERNAL` でも details が空文字列や `"malformed gRPC response frame"` に戻る、または unary と server streaming の片方だけ異なる、という ext-grpc/gax 互換上の回帰を検出できない。
- Recommended fix: `poc/test-server/main.go` の non-gRPC h2c fixture か raw lifecycle fixture に clean missing-trailers response を追加し、`tests/phpt/022-error-and-http-validation.phpt` で unary と server streaming の両方について `STATUS_INTERNAL` と `"server closed the stream without sending trailers"` を assert する。必要なら `docs/verification/test-fixtures.md` / verification matrix も fixture 追加に合わせて更新する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `review only`
- Notes: compressed flag と unsupported `grpc-encoding` の code/details は、対象 PHPT と `CompressionTest.php` で unary / server streaming ともに十分固定されている。`NGHTTP2_HTTP_1_1_REQUIRED` は pure status taxonomy mapping として C unit coverage で妥当と判断した。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `1`
- Low: `none`
- Design Decision: `none`
