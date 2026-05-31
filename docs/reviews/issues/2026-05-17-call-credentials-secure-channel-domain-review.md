# CallCredentials secure-channel domain review 2026-05-17

## Scope

- `ext/grpc/bridge.c`
- `ext/grpc/tests/004-object-lifecycle.phpt`
- `ext/grpc/tests/023-metadata-and-call-credentials.phpt`
- `ext/grpc/tests/026-franken-go-backend.phpt`
- `docs/verification/protocol-model-review-guide.md`
- `docs/issues/closed/2026-05-17-call-credentials-insecure-security.md`

## Reviewer Role

- HTTP/2/gRPC domain model reviewer for CallCredentials secure-channel boundary

## Review Prompt Summary

- CallCredentialsをrequest metadata sourceとして扱う境界、secure vs insecure channel、unary/server-streaming/franken-go lifecycle、error taxonomy/status、callback実行順序、public/internal境界、production/test境界を確認した。

## Issues

### REVIEW-20260517-001: franken-go insecure CallCredentials lifecycle is unverified

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2/gRPC domain model reviewer for CallCredentials secure-channel boundary`
- Finding: `ext/grpc/bridge.c` はfranken-go unary/server-streamingにもinsecure CallCredentials拒否を追加しているが、対象PHPTはnative unary/TLS unary/native server-streamingのみを固定しており、franken-go backendでcallback未実行・backend未委譲・status surfaceが同じになることを検証していない。
- Evidence: `ext/grpc/bridge.c` `grpc_lite_perform_call_unary_franken_go()` / `grpc_lite_open_call_stream_franken_go()`、`ext/grpc/tests/023-metadata-and-call-credentials.phpt`、`ext/grpc/tests/026-franken-go-backend.phpt`
- Expected model: CallCredentialsのsecure-channel gateはHTTP/2 transportだけでなく、franken-go delegateへrequest metadataを渡す前のgRPC Call境界で同一に働く。insecure channelではcallbackを実行せず、RPC/backendへ委譲せず、`UNAUTHENTICATED` statusをPHP Call surfaceへ返す。
- Why it matters: franken-go backendはproduction側のoptional backendであり、native pathだけのPHPTだと将来のbridge再編でbackend delegate前のsecurity gateが外れても検出できない。
- Recommended fix: `ext/grpc/tests/026-franken-go-backend.phpt` か専用PHPTに、insecure channel + CallCredentialsのfranken-go unary/server-streamingケースを追加し、callback未実行、`FrankenGrpc\UnaryCall::start()` / `ServerStreamingCall::start()` 未到達、`STATUS_UNAUTHENTICATED` を確認する。
- Fix summary: `franken-go unary/server-streaming PHPTにinsecure CallCredentialsケースを追加し、callback未実行、backend start未到達、UNAUTHENTICATED statusを固定した。`
- Fix commit: `pending`
- Verification: `./tools/test/check-phpt.sh: 15/15 PASS`
- Notes: native unary/server-streamingの実装モデル自体は、callback merge前に`grpc_lite_mark_call_failed()`でcall-semantic failureへ畳み、transport/cacheに触らないため、secure-channel boundaryとerror taxonomyに合っている。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`

## Re-review Result 2026-05-17

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
- Notes: fixes preserve CallCredentials as a request metadata source, enforce the secure-channel boundary before callback execution and transport/backend delegation, keep native/franken-go unary/server-streaming lifecycle on the PHP Call status surface, and do not leak test fixtures into production code.

## Verification Summary

- `grpc_lite_fail_if_call_credentials_require_secure_channel()` はCallCredentials callbackをrequest metadata sourceとして実行する前にinsecure channelを拒否し、callback orderingとsecure-channel boundaryを保っている。
- native unaryは`UNAUTHENTICATED` statusを作って`unary_performed`を立て、HTTP/2 connection/cacheへ進まない。
- native server-streamingはstream resourceを開かずに`status_ready`を立て、次のpull/status取得で同じPHP status surfaceへ収束する。
- franken-go unary/server-streamingもPHPTでcallback未実行、backend `start()` 未到達、`UNAUTHENTICATED` statusを固定している。
- public API surfaceには内部transport概念を露出しておらず、test-only certificate/backend fixtureはPHPT内に閉じている。
