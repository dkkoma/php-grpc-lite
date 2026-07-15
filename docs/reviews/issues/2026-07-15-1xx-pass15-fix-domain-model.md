# 1xx informational response pass 15 fix domain model review 2026-07-15

## Scope

- HEAD `6470c7f` とcurrent working treeのpass-15未コミット差分
- `src/response_header_phase.[ch]`
- `src/grpc_exchange_state.h`
- `src/status_core.c`
- `src/transport.[ch]`
- `src/diagnostic/bench.c`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `poc/test-server/main.go`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- `docs/SPEC.md`
- `docs/design/grpc-call-exchange-state.md`
- `docs/design/protocol-classification-boundary.md`
- `docs/guides/code-reading-guide.md`
- `docs/verification/compatibility-control-checklist.md`
- `docs/verification/test-fixtures.md`
- `docs/verification/verification-matrix.md`
- `docs/issues/open/2026-07-10-informational-1xx-response-handling.md`
- `docs/reviews/issues/2026-07-15-1xx-adversarial-consolidated-pass15.md`

## Reviewer Role

- HTTP/2 / gRPC domain model gate reviewer（pass-15 fix）

## Review Prompt Summary

- `AWAITING_STATUS`でregular fieldが`:status`より先行した時点のsemantic classification、END_HEADERS未完了blockのstream / connection lifecycle、wire budgetとprotocol errorのpriorityを確認した。
- production / raw diagnosticがcall-local classificationとRST actionを共有しつつ、persistent connection quarantineとdisposable fdのnonblocking finishを各ownership境界に保持しているか確認した。
- normal / invalid header producer、unary / server streaming、peer-observed RST、fresh follow-up、diagnostic fd modeのoracleが修正前の各退行を識別するか確認した。
- current-state specification / design / code-reading / verification docsが実装とfixtureの最終contractを現在形で表しているか確認した。

## Issues

### Blocker

- none

### High

- none

### Medium

- none

### Low

- none

### Design Decision

- none

## Domain Gate Checks

- Semantic phase / naming: `GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS`はfinal response未確定blockでmandatory `:status`を待つstateとして維持される。`grpc_protocol_reject_response_regular_header_before_status()`はphase transitionを装わず、normal / invalidの区別に依存しないregular-field ordering violationをcall-local protocol classificationへ写像する。最初の違反で`response_header_block_protocol_valid=false`となるため、同じblockの後続fieldがsemantic metadataやstatusへcommitされない。
- Budget / protocol priority: production / diagnosticのnormal callbackとinvalid callbackはいずれもwire header accountingを先に実行する。超過時は`metadata_too_large`と`NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE`をそのまま返し、regular-before-status helperへ進まないため、同じfieldで競合する場合も`RESOURCE_EXHAUSTED` + `RST_STREAM(CANCEL)`がprimary ownerとなる。budget内fieldでordering violationが確定した場合だけ`response_header_protocol_error` + `RST_STREAM(PROTOCOL_ERROR)`へ進む。
- Call / stream / connection ownership: ordering violationのpublic taxonomyはcall-local `response_header_protocol_error`が所有し、RST codeはreject helperが選ぶ。END_HEADERS未完了判定、`response_header_block_incomplete`、target RST submit、connectionのclose-after-pending-flush遷移は既存`grpc_protocol_apply_response_header_terminal_action()`へ集約され、parallelなquarantine pathを追加していない。productionはpending control frameを固定grace内でflushした後にconnectionをdead化し、既存siblingは追加session / socket I/Oなしで`UNAVAILABLE`、follow-upはfresh connectionへ移る。
- Complete / incomplete lifecycle: END_HEADERS付きregular-before-status blockはcall-local protocol errorを確定したうえでnghttp2のblock completion rejectionへ委ね、connectionがusableなら再利用可能である。END_HEADERSなしblockだけがconnection-global HPACK decoder同期喪失としてterminal quarantineへ進むため、stream-local malformed responseとconnection-terminal incomplete blockを同一scopeへ畳んでいない。
- Unary / server streaming: 両call kindは同じheader callback、status priority、connection terminal actionを共有する。PHPT 042は両方についてnormal / invalid regular-before-statusとinvalid incomplete budgetのfinite outcome、primary details、exact caller-selected RST、fresh follow-upを確認し、server streaming固有のresponse countだけをcall orchestration側で区別する。
- Production / diagnostic boundary: raw diagnosticはproductionのphase / budget / reject / RST helperを共有するが、persistent `h2_connection` lifecycleを所有しない。`bench_finish_response_header_terminal_action()`はshared `response_header_block_incomplete`を読み、one-shot fdだけをnonblockingへ切り替える。`incomplete_header_fd_nonblocking`はbench diagnostic resultに限定され、production `Grpc\` APIやconnection modelへtest-only stateを追加していない。
- Oracle truthfulness: fixtureはvalid regular fieldとNUL-bearing invalid regular fieldを別controlで同じincomplete missing-status sequenceへ載せ、normal / invalid callback wiringを分離する。invalid incomplete budget controlは`:status: 103`の1 entry後にinvalid field 129個を送り、callback 128回目のoverflowと129個目のcutoffを識別する。PHPT 042のauthority-keyed peer marker、exact RST code、connection preface数、fresh follow-upはproduction terminal ownershipを、PHPT 043のcallback countとdefault-blocking fdの実`O_NONBLOCK`観測はdiagnostic producer / consumerをそれぞれ識別する。
- Documentation: SPEC、exchange-state map、classification boundary、code-reading guide、fixture catalog、verification matrix、compatibility checklistは、regular-before-statusの早期`INTERNAL`、budget-first priority、caller-selected RST、complete / incomplete connection reuse差、diagnostic nonblocking observationをcurrent implementationと同じscopeで記述する。

## Verification

- current diffとadjacent phase / status / connection lifecycle codeを静的照合
- production / diagnosticのnormal・invalid header callbackからwire budget owner、regular-before-status reject helper、shared incomplete-block terminal actionまでのcall pathを静的照合
- unary / server streamingのclose-after-pending-flush、dead後I/O gate、target / sibling taxonomyを静的照合
- raw fixtureのcompleted 103、fragmented regular field、invalid-field entry overflow、peer RST marker、authority-keyed fresh follow-up sequenceを静的照合
- PHPT 042 / 043のstatus / details、message count、exact RST、connection preface、invalid callback count、実fd `O_NONBLOCK` oracleを静的照合
- `git diff --check` PASS
- runtime suiteはreview-only laneでは未実行（implementation agentのrequired verification laneで実行）

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
