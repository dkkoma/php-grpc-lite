# 1xx pass-3 fix HTTP/2 / gRPC domain model review pass 4 2026-07-15

## Scope

- pass-3 finding対応後の未コミット差分
- `src/response_header_phase.[ch]`
- `src/transport_core.[ch]`
- `src/transport.[ch]`
- `src/diagnostic/bench.c`
- `poc/test-server/main.go`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- `tests/unit/test_transport_core.c`
- 1xx issue、pass-3 review records、関連verification docs

## Reviewer Role

- HTTP/2 / gRPC domain model gate reviewer (pass 4)

## Review Prompt Summary

- response semantic phaseとwire header budgetの責務、valid / invalid header callback accounting、stream-local `RST_STREAM(CANCEL)`とcallback return、production / diagnostic parityを独立に確認した。
- pushed-stream ownership、`grpc-status-details-bin` terminal lifecycle、fixture oracleのfield-class / CANCEL識別力、valid informational / iteration reset control、positive controlで顕在化したbench raw send経路も確認した。
- `docs/verification/protocol-model-review-guide.md` に従い、call-local state、HTTP/2 stream ownership、resource failure taxonomy、production / diagnostic boundaryをgate観点とした。

## Issues

### REVIEW-20260715-001: invalid-header budget超過後もcallbackのtemporal failureを握り潰して同一blockの処理を継続する

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC domain model gate reviewer (pass 4)`
- Finding: normal / invalid regular fieldの双方は共有wire budgetへ一度だけ計上され、超過時のcall-local classificationと`RST_STREAM(CANCEL)` queueも行われる。一方、production / diagnosticのinvalid-header callbackだけは共有ownerが返す`NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE`を0へ変換するため、nghttp2へ「このstreamのheader callback処理をここで止める」という結果を返していない。したがって1つのHEADERS block内でbudget超過fieldより後ろに大量のinvalid regular fieldがある場合、CANCELはpendingでも残りのinvalid-header callbackが呼ばれ続け、valid field経路と異なるresource boundaryになる。
- Evidence: pure accounting owner `grpc_response_header_budget_account_field()` は `src/transport_core.c:112-129`、call-local classification / CANCEL owner `grpc_protocol_account_response_header_field()` は `src/transport.c:2216-2241` にあり、超過時はCANCELをsubmitした後に`NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE`を返す。normal callbackはその値をそのまま返す (`src/transport.c:2257-2261`, `src/diagnostic/bench.c:356-360`)。しかしinvalid callbackはproductionで `rv == NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE ? 0 : rv` (`src/transport.c:2382-2388`)、diagnosticで同じ場合に0 (`src/diagnostic/bench.c:487-495`) を返す。nghttp2の[official callback contract](https://nghttp2.org/documentation/nghttp2.h.html)は、invalid fieldをignoreしてstreamを継続する場合は0、desired error codeを`nghttp2_submit_rst_stream()`で選んだ場合もstreamをresetするには`NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE`を返す、と区別している。v1.69.0の[`inflate_header_block()`](https://github.com/nghttp2/nghttp2/blob/v1.69.0/lib/nghttp2_session.c)もinvalid-header callbackのTEMPORALでそのblockのcallback loopから戻る。既存invalid entry / byte fixtureは最終的なCANCEL受信を観測するが、超過後の同一block callback打ち切りは観測しない。
- Expected model: wire budgetのpure ownerはfield accounting decisionだけを持ち、nghttp2 orchestration ownerは超過をcall-local `metadata_too_large`、exact `RST_STREAM(CANCEL)`、`NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE`の3点へ一貫して写像する。fieldがsemantic metadataとしてvalidかignore対象かはこのresource stop boundaryを変えない。invalid-frame observerが同じTEMPORALを観測する場合は、既に`metadata_too_large`が確定したcallを`response_header_protocol_error`へ上書きせず、RESOURCE_EXHAUSTED分類を保存する。
- Why it matters: hard budgetは単なる最終status flagではなく、peer由来の追加header processingをstream-localに停止する境界である。現在もCANCELは送出されるためconnection全体は壊さないが、invalid regular fieldだけは同一blockの残りapplication callback / HTTP field validation workをbudget後も継続し、production / diagnosticおよびvalid / invalid callback間で停止点が一致しない。
- Recommended fix: production / diagnosticのinvalid-header callbackからTEMPORALをそのまま返す。両`on_invalid_frame_recv_callback`は`metadata_too_large`が既に立っている対象streamではprotocol-error markerを追加しないようにし、CANCEL / RESOURCE_EXHAUSTEDを優先する。invalid-field overflow後の同一block callback cutoffを識別するfocused oracleを追加し、既存のunary / server streaming RESOURCE_EXHAUSTED、exact CANCEL、same-connection follow-up、bench budget rejectionも再実行する。
- Fix summary: production / diagnosticのinvalid-header callbackが共有accounting ownerのreturnを変換せず、そのまま返すようにした。両invalid-frame observerは対象callで`metadata_too_large`が確定済みならprotocol-error markerを追加せずreturnし、explicit `RST_STREAM(CANCEL)`、TEMPORAL callback stop、RESOURCE_EXHAUSTED分類を同じstream-local failureとして保存する。
- Fix commit: `pending`
- Verification: `修正後のsrc/transport.c:2366-2402とsrc/diagnostic/bench.c:476-502を再レビューし、budget内invalid fieldは0、超過時はshared ownerからTEMPORAL、observerはmetadata_too_large優先となることを確認。./tools/test/check-phpt.sh 28/28 PASS（PHPT 042のinvalid entry / byte unary・server streaming RESOURCE_EXHAUSTED、exact CANCEL、same-connection follow-upとPHPT 043のdiagnostic budget / ownershipを含む）。`
- Notes: `invalid regular fieldをsemantic stateへ反映せずignoreする設計自体は妥当であり、本findingはbudget内fieldのreturn 0を問題にしない。budget超過時だけ、既にqueueしたCANCELとcallback returnの意味が分離していた点を対象とした。修正後はnghttp2 contractどおり両者が一致した。`

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
