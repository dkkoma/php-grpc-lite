# PR #29 第五パス C ownership / lifetime review 2026-07-12

## Scope

- `2af2d58..287bc939`（実装commit `3e128d4`）
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `src/transport.c`
- `src/diagnostic/bench.c`
- `src/wrapper_adapter.c`

## Reviewer Role

- low-level C / persistent-cache ownership safety adversary

## Review Prompt Summary

- fatal submit時の即時cache detachが、全callerのraw `h2_connection *` lifetime、owner count、最終destroy、replacement entryを安全に保つか確認した。
- review subagentが記録前に実行制限で終了したため、親agentが返されたstatic findingと独立ASan結果を本fileへ転記した。

## Issues

### REVIEW-20260712-001: unary coreが破棄したconnectionをdiagnostic callerがfailure後にdereferenceする

- Severity: `High`
- Status: `Open`
- Reviewer role: `low-level C / persistent-cache ownership safety adversary`
- Finding: `nghttp2_submit_request()` fatal時、unary coreはconnectionをdead化してcacheからdetachし、call ownerをclearした後、owner count 0ならconnectionを破棄して`FAILURE`を返す。`--enable-grpc-bench`のdiagnostic callerはfailure後も同じraw pointerを`connection_usable()`へ渡すためuse-after-freeになる。
- Evidence: `src/unary_call.c:158-173`、`src/diagnostic/bench.c:1878-1883`。`--enable-grpc --enable-grpc-bench --enable-grpc-test-fault`のASan/UBSan buildで`GRPC_LITE_TEST_FAULT=submit-request-fatal`を設定しdiagnostic `grpc_lite_unary()`を1回呼ぶと、free=`src/unary_call.c:171` / `src/transport.c:221`、read=`src/diagnostic/bench.c:1879` / `src/transport.c:873`としてdeterministicにabortした。
- Expected model: connectionの最後のownerが破棄した後は、callee/caller境界を越えてraw pointerを再参照しない。cache detachと最終destroyの責務は一箇所に置き、calleeがpointerを消費するならreturn contractで全callerへ明示する。
- Why it matters: diagnostic/benchmark buildでheap-use-after-free、crashまたは未定義動作になる。real `nghttp2_submit_request()` fatalでも同じbranchへ到達する。default production buildはbench entrypointを含まないが、repositoryが維持するdiagnostic build boundaryのmemory safety regressionであり、今回の即時evict修正が直接導入した。
- Recommended fix: diagnostic callerのfailure branchから`connection_usable()` / `remove_unusable_persistent_connection()`を除き、calleeに消費され得るpointerへ触れない。またはcoreがdestroyせず、cache keyを持つcaller側へdetach/destroyを一貫して委譲する。bench + test-fault併用ASan regressionを追加する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: ASan/UBSan combined buildで再現。標準NTS PHPTは24/24 PASSだがbench entrypointをcompileしないため本経路を通らない。
- Notes: production wrapperは`src/wrapper_adapter.c:516-518`でcore failureを即returnしpointerを再参照しない。server streaming openもraw connection pointerをcallerへ返さずcallee内でcleanupを完結するため、この具体的な再参照はdiagnostic unary callerに限定される。

## Review Result

- Blocker: none
- High: 1 (Open)
- Medium: none
- Low: none
- Design Decision: none
