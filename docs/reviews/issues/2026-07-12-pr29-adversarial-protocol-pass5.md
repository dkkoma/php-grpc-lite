# PR #29 第五パス HTTP/2 / gRPC domain model review 2026-07-12

## Scope

- `2af2d58..287bc939`（実装commit `3e128d4`）
- `config.m4`
- `grpc.c`
- `src/transport.c`
- `src/transport.h`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `src/diagnostic/bench.c`
- `src/status_core.c`
- `src/grpc_exchange_state.h`
- `tests/phpt/024-control-semantics.phpt`
- `tests/phpt/038-fatal-rst-submit-marks-connection-dead.phpt`
- `tests/phpt/039-fatal-submit-request-marks-connection-dead.phpt`

## Reviewer Role

- HTTP/2 / gRPC lifecycle・status taxonomy・connection/cache ownership adversary

## Review Prompt Summary

- 第四パスの3指摘に対する修正を、partial-frame TCP/TLS breakとclean END_STREAM truncationの区別、transparent retry、dead connectionのcache/owner lifecycle、production/test boundaryの観点から再確認する。
- 修正で新たに生じたconnection raw pointer lifetimeと、RST fatal時のcall semantic / connection failure priorityも確認する。

## Issues

### REVIEW-20260712-001: unary coreが破棄したdetached connectionをdiagnostic callerがfailure後に再参照する

- Severity: `High`
- Status: `Open`
- Reviewer role: `HTTP/2 / gRPC lifecycle・connection/cache ownership adversary`
- Finding: `nghttp2_submit_request()` fatal時、unary coreはconnectionをdead化してpersistent cacheからdetachし、owner clear後に`destroy_detached_connection_if_unowned()`でconnection自体を破棄してから`FAILURE`を返す。一方、`--enable-grpc-bench`のdiagnostic callerはfailure後も同じraw pointerを`connection_usable()`へ渡す。calleeがconnection lifetimeを消費したことをreturn contractで表さないまま、caller側のcache cleanup責務と競合している。
- Evidence: `src/unary_call.c:155-173`（`detach_persistent_connection_by_ptr()`、owner clear、最終destroy）、`src/diagnostic/bench.c:1878-1883`（failure後の`connection_usable(connection)`）。同じpass5のASan/UBSan diagnostic probeは、`--enable-grpc --enable-grpc-bench --enable-grpc-test-fault` + `GRPC_LITE_TEST_FAULT=submit-request-fatal`の1 unary callで、free=`src/unary_call.c:171`、read=`src/diagnostic/bench.c:1879`のheap-use-after-freeを再現している（`docs/reviews/issues/2026-07-12-pr29-adversarial-test-pass5.md`）。production wrapperは`src/wrapper_adapter.c:516-518`でfailureを即returnするため、この再参照をしない。
- Expected model: persistent cache entryのdetach、connectionの最後のowner解放、raw pointerの最終destroyには1つの明確なownerを置く。coreがconnectionを消費して返るなら全callerがそのpointerへ触れないことを型/結果で伝え、callerがkeyを使ってcleanupするならcoreはpointerを破棄しない。
- Why it matters: diagnostic/benchmark buildでdeterministicなheap-use-after-freeとなり、crashまたは未定義動作を起こす。production default surfaceではないが、repositoryが明示的に維持するdiagnostic boundaryであり、今回のdead-entry即時evict修正が導入したlifetime regressionである。
- Recommended fix: unary coreと2 callerのfailure contractを統一する。例えばcoreはfatal時にdeadだけを確定して返し、cache keyを持つproduction wrapper / diagnostic callerがfailure branchで`remove_unusable_persistent_connection()`を行う。またはcore resultへ`connection_consumed`を明示し、消費済みpointerをcallerが再参照しないようにする。bench + test-fault併用ASan regressionを追加する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: source上のowner/caller chainを確認。動的再現は上記pass5 test reviewと突合した。
- Notes: server streaming openはconnection取得からfatal cleanupまで同じcalleeが所有し、diagnostic callerへraw connection pointerを返さないため、この具体的UAFはunary diagnostic pathに限定される。

### REVIEW-20260712-002: RST submit fatal時にdeadlineのstatus detailsがconnection cleanup failureへ上書きされる

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `HTTP/2 / gRPC lifecycle・status taxonomy adversary`
- Finding: server streamingでmessageを1件deliveryした後、次のpull前にdeadlineが切れると`timed_out=true`を先に確定してRST_STREAM(CANCEL)を送る。RST submitがfatalならconnectionに`nghttp2 error: Out of memory`が記録される。status code resolverは`timed_out`を最優先して`DEADLINE_EXCEEDED`を返すが、details resolverはconnection error detailをcode固有のdeadline detailsより先に返すため、PHP-visible statusが`code=4 / details="nghttp2 error: Out of memory"`になる。通常のrecv poll timeoutではcall側にdeadline detailsが先にsnapshotされるため、同じdeadlineがcall kindとGenerator pull timingで異なるdetailsを返す。
- Evidence: deadline確定とcancelは`src/server_streaming_call.c:282-286`、fatal RSTによるconnection detailは`src/transport.c:325-352,965-972`、code priorityは`src/status_core.c:9-10`、details priorityは`src/transport.c:2370-2398`。test-fault buildのfocused probe（2-message server stream、1件目を受信、deadline経過後に次pull、`GRPC_LITE_TEST_FAULT=rst-submit-fatal`）で`int(4)`と`"nghttp2 error: Out of memory"`を再現した。PHPT 038は`tests/phpt/038-fatal-rst-submit-marks-connection-dead.phpt:43-48`で「timed_outがconnection failureより優先」と説明するがcodeだけをassertする。
- Expected model: deadline / caller cancel / response-policyのようにcall-local semanticがconnection cleanup failureより先に確定した場合、PHP-visible statusのcodeとdetailsは同じprimary outcomeを表す。RST送出失敗はconnectionをdead化・detachするsecondary lifecycle failureとしてtrace/diagnosticへ残してよいが、primary deadline detailsを置き換えない。
- Why it matters: retry判断に使うcodeは維持されるものの、public Status objectが原因の異なるcode/details pairになり、同じdeadlineがpoll位置だけで異なる診断を返す。今回の元issueが追加した「deadline cancelのRST失敗はconnection破棄へfallbackしつつcallはDEADLINE_EXCEEDED」という契約をdetails側が満たしていない。
- Recommended fix: details resolutionでも少なくとも`call->timed_out`（および既に確定したresponse-policy flag）をconnection error detailより先に評価する。RST submit/flush failure detailが必要ならcall status detailsではなくtraceまたはsecondary diagnostic fieldへ残す。PHPT 038へserver streamingのbetween-pull deadlineを追加し、exact code/details、fresh connection、fatal後のsession再駆動なしをassertする。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: HEAD `287bc939`のfocused dynamic probeとcode-path reviewで再現。unaryのrecv-timeout経路はtimeout detailをcancel前にcallへsnapshotするため`HTTP/2 transport deadline exceeded`を維持することも確認した。
- Notes: 同じpriority splitはscope-adjacentなresponse-policyでも観測できる。message-too-large + fatal callback RSTではserver streamingが`code=RESOURCE_EXHAUSTED / details=nghttp2 OOM`、unaryはpolicy statusを構築せず`nghttp2_session_mem_recv failed` exceptionになる。後者は元issueのdeadline/RST connection reuseそのものではなく、fatal-remediation seamが露出したmessage-limit policyの既知隣接問題なので、本レビューでは別のmerge findingとして加算していない。

## Adequate Fixes Confirmed

- `--enable-grpc-test-fault`はdefault offで、fault state/functionは`PHP_GRPC_LITE_ENABLE_TEST_FAULT` guard内、production call siteはconstant `false`になる。MINIT copy、read-only publication、exact comma token matchも前回のraw `getenv()` pointer lifetime問題を解消している。
- fixture `:50057`のTCP break mid-messageはunary / server streamingとも`UNAVAILABLE`となり、streamingのpartial parser stateが`INTERNAL`へ上書きしない。clean HTTP/2 END_STREAM mid-messageは既存PHPT 022で両call kindとも`INTERNAL`を維持する。
- connection breakはREFUSED/GOAWAY outcomeを立てないためtransparent retry対象にならず、既存のunprocessed保証付きGOAWAY / RST_STREAM(REFUSED_STREAM) retry predicateも変更されていない。
- submit fatal時のcache detachは、production unary wrapper、server streaming、およびshared ownerが残る場合の`stream_owner_count` delayed destroyでは妥当である。問題は上記diagnostic callerとのraw pointer failure contractに限定される。

## Review Result

- Blocker: none
- High: 1 (Open)
- Medium: 1 (Open)
- Low: none
- Design Decision: none
