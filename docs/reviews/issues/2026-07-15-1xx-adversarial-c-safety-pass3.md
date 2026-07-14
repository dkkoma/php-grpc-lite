# 1xx informational response adversarial C safety pass 3 2026-07-15

## Scope

- Commits `20c2dc0` and `6a4902f`
- `src/response_header_phase.h`
- `src/response_header_phase.c`
- `src/grpc_exchange_state.h`
- `src/transport.c`
- `src/status_core.c`
- `src/diagnostic/bench.c`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `src/wrapper_adapter.c`
- `poc/test-server/main.go`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- pass 1 adversarial review records 3 files
- `docs/reviews/issues/2026-07-15-1xx-fix-domain-model-pass2.md`
- `docs/issues/open/2026-07-10-informational-1xx-response-handling.md`

## Reviewer Role

- C safety / lifetime adversary

## Review Prompt Summary

- pass 1の全findingとpass 2 recordを先に読み、`6a4902f` が各failure modeを実際に閉じたかを、C memory safety、`grpc_call` lifetime、stream user-data、nghttp2 callback ordering、`:status` parse、wire header budget、production / bench parityの観点から静的に敵対的レビューした。
- unary / server streaming / transparent retry / persistent connection reuse / deadline・cancel後のcall state初期化と解放を列挙して確認した。テストおよびDockerは指示どおり実行せず、runtime確認が必要なfindingにはexact wire probeを記載した。

## Issues

### REVIEW-20260715-001: nghttp2 1.67以降でsilent-ignoreされるinvalid regular fieldがwire header hard budgetを迂回する

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `C safety / lifetime adversary`
- Finding: pass 1 C-safety `REVIEW-20260715-001` の修正はnormal `on_header_callback()`へ到達するfieldだけをwire budgetへ計上する。nghttp2 1.67以降はapplicationがinvalid-header callbackを登録しない場合、invalid regular header fieldをstream errorにせずsilent-ignoreするため、peerはHPACK decode済みfieldを `wire_response_header_entry_count` / `wire_response_header_bytes` に一度も反映させず、cleanなfinal responseまで完了できる。全decoded response fieldのhard work budgetという修正後modelがdependency versionによって成立しない。
- Evidence: `src/transport.c:421-435` の `configure_callbacks()` はnormal header callbackとinvalid-frame callbackを登録するが、`nghttp2_session_callbacks_set_on_invalid_header_callback{,2}()` は登録しない。accountingは `src/transport.c:2213-2237` の `grpc_protocol_account_response_header_field()` をnormal `on_header_callback()` の `:2253-2260` から呼ぶ経路だけにある。nghttp2 1.67の[公式release note](https://nghttp2.org/blog/2025/09/02/nghttp2-v1-67-0/)は、両invalid-header callbackが未登録の場合の挙動をstream errorからsilent ignoreへ変更したと明記し、現行の[callback contract](https://nghttp2.org/documentation/types.html)もinvalid regular fieldがnormal callbackとは別経路に入ることを定義する。`config.m4:6` は `libnghttp2` のversionを制約せず、README / Docker / release buildもdistroのunversioned `libnghttp2-dev` を使うため、1.67以降はsupported source buildの範囲である。exact byte probeはraw fixtureから `HEADERS(:status: 103, x-ignored: <leading SP + 2048 bytes>, END_HEADERS)` を送り、その後cleanな `:status: 200` / `content-type: application/grpc`、DATA、`grpc-status: 0` END_STREAMを送る。clientに `grpc.absolute_max_metadata_size=1024` を設定しても現実装はinvalid fieldをcountせずOKになる。entry probeは1つの103 blockへleading SP valueを持つ128個の小さいregular fieldを入れ、`:status` と合わせて128-entry capを超えさせる。nghttp2 1.66以前では同じfieldがstream errorになるため、成功迂回probeの対象は1.67以降である。
- Expected model: semantic metadataとしてignoreされるfieldを含め、nghttp2がHPACK decodeしてapplication policy上無視する全response regular fieldをcall-local wire work budgetへ一度だけ計上する。configured byte limitまたは128-entry limit超過はnghttp2 versionによらず `metadata_too_large`、`RST_STREAM(CANCEL)`、`RESOURCE_EXHAUSTED` へ分類する。
- Why it matters: production RPCでuserが設定したmetadata hard limitと固定entry capをpeerが迂回でき、PHP workerへその上限を超えるHPACK decode / validation workを負わせられる。1KiB capのような小さい防御設定が新しいnghttp2ほど効かなくなるdependency-sensitiveなresource-control regressionであり、単なるmetadata visibility差ではない。
- Recommended fix: production callbacksへinvalid regular header callbackを登録し、stream user-dataから同じ `grpc_call` をNULL-safeに引いたうえで、name/valueを既存のoverflow-safe accounting helperへ渡す。既存compatibilityどおりfield自体をignoreする場合はbudget処理後に0を返し、helperのfatal returnだけはcallback failureとして伝播する。byte / entry双方のraw probeと、overflow後のsame-connection follow-up成功をnghttp2 1.67以降でも固定する。
- Fix summary: productionへinvalid regular header callbackを登録し、normal / invalid response fieldを共有accounting ownerとoverflow-safeなpure helperへ通した。budget超過は`metadata_too_large`、body discard、`RST_STREAM(CANCEL)`へ分類し、invalid fieldはsemantic stateへ反映しない。NULを含むinvalid regular fieldのentry / byte raw controlsを追加した。
- Fix commit: `pending`
- Verification: Dockerのlibnghttp2 1.64でcallback経路を実行し、PHPT 042のinvalid regular entry / byte controlsがunary / server streaming双方で`RESOURCE_EXHAUSTED`、対象streamのCANCEL観測後のsame-connection follow-upがOKとなることを確認。C unitでentry / byte / arithmetic overflow境界を確認。PHPT 28/28、C unit 4/4、static analysis PASS。
- Notes: `pass 1 C-safety REVIEW-20260715-001のvalid informational field経路は修正済みだが、normal callbackを迂回するdecoded fieldが残るため、新しいfindingとして記録する。` protocol `REVIEW-20260715-001`と同一defectとして一度だけ修正した。leading SPではなくNUL-bearing invalid regular fieldを採用したが、同じnghttp2 invalid-header callback経路を通る。budget未満はignoreし、超過時は明示CANCELをqueueした後もinvalid-frame observerへprotocol errorを誤記録させないcallback returnへreconcileした。

### REVIEW-20260715-002: `grpc-status-details-bin`がbenchのterminal status gateを迂回する

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `C safety / lifetime adversary`
- Finding: pass 1 protocol `REVIEW-20260715-003` のbench false-success修正は `grpc-status` と `grpc-message` だけをshared END_STREAM predicateへ通し、同じstatus fieldである `grpc-status-details-bin` をgeneric metadataとして処理する。post-1xx `FINAL_INITIAL` のnon-terminal blockにdetails-binだけを置くと `initial_grpc_status_seen` / `invalid_grpc_status` が立たず、後続のvalid `grpc-status: 0` trailerでmalformed sampleをbench成功件数へ加算できる。
- Evidence: productionの `src/transport.c:2324-2333` は `grpc-status-details-bin` でも `initial_grpc_status_seen` を設定し、`grpc_response_header_phase_allows_status_fields()` がfalseならcommitせず `invalid_grpc_status` とする。一方 `src/diagnostic/bench.c:355-390` のstatus branchは `grpc-status` / `grpc-message` だけで、details-binはgeneric metadataへfall throughする。frame-end validation `src/diagnostic/bench.c:540-547` は `initial_grpc_status_seen` に依存し、success gate `:1700-1707` は後続terminal status 0を受理する。pass 2 record `docs/reviews/issues/2026-07-15-1xx-fix-domain-model-pass2.md:41,45` は3 status fieldすべてのshared predicate適用とproduction / diagnostic parityを確認済みとしているが、PHPT 043とraw control `post-informational-nonterminal-status` は `grpc-status` だけを送る。exact probeは `103 HEADERS` → non-END_STREAM `HEADERS(:status 200, content-type application/grpc, grpc-status-details-bin: AA==)` → valid gRPC DATA → END_STREAM `HEADERS(grpc-status: 0)`。productionは `invalid_grpc_status` によりUNKNOWN、現bench batchは `ok=1 / failed=0` になり得る。
- Expected model: `grpc-status` / `grpc-message` / `grpc-status-details-bin` はproductionとbenchの双方で同じterminal-block commit gateを通り、FINAL_INITIALまたはTRAILINGのEND_STREAM付きblockでだけstatus semantic stateへ反映される。
- Why it matters: productionが拒否するmalformed responseをbenchmarkだけが成功sampleとしてlatency / throughputへ混ぜる。pass 1 Medium findingと同じmeasurement integrity failureがheader名を替えるだけで残り、修正済みgateの識別力もPHPT 043の1 fieldに限定されている。
- Recommended fix: bench callbackへproductionと同じ `grpc-status-details-bin` branchを追加し、FINAL_INITIALでは `initial_grpc_status_seen` を設定してshared predicateでgateし、non-terminalなら `invalid_grpc_status` / discardへ落とす。上記raw controlをPHPT 043へ追加し、production UNKNOWNとbench `ok=0 / failed=1` の両方を固定する。
- Fix summary: benchのconsumer field setへ`grpc-status-details-bin`を追加し、FINAL_INITIALでのstatus観測とshared END_STREAM predicateをproductionと一致させた。non-terminal detailsは`invalid_grpc_status` / discardへ分類する。
- Fix commit: `pending`
- Verification: PHPT 042でproduction unary / server streamingが`UNKNOWN / invalid grpc-status trailer`、PHPT 043でbenchが`ok=0 / failed=1`となることを同じexact raw sequenceで確認。PHPT 28/28 PASS。
- Notes: `pass 1 protocol REVIEW-20260715-003の修正不足を、未検証の別status fieldで再現する新しいfinding。` protocol `REVIEW-20260715-003`と重複するためstatus field lifecycleを一度だけ修正した。

### REVIEW-20260715-003: bench diagnosticではinformational wire header budgetが適用されない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `C safety / lifetime adversary`
- Finding: productionへ追加したcall-local wire header entry/byte budgetはbench callbackへ存在せず、benchはvalidなinformational fieldをphase early returnで無制限に捨てる。共有 `grpc_call` にcounter fieldはあるがbatch iterationでresetされず、`max_response_metadata_bytes` もdiagnostic用に初期化されないため、「production / diagnosticがphase semanticsを共有する」という修正後modelのうちresource boundaryだけが欠落している。
- Evidence: productionは `src/transport.c:2253-2260` でsemantic phase判定より前に全normal HEADERS fieldをaccountするが、`src/diagnostic/bench.c:319-426` には対応する呼び出しがない。iteration reset `src/diagnostic/bench.c:1451-1465` も `wire_response_header_entry_count`、`wire_response_header_bytes`、`metadata_too_large` をresetしない。既存raw control `poc/test-server/main.go:704-710` は129個の103 blockへvalid `x-info` を載せた後clean OKを返し、PHPT 042 `:106-110` はproductionの `RESOURCE_EXHAUSTED` だけを検証する。bench success gate `src/diagnostic/bench.c:1700-1707` はwire budget / `metadata_too_large` を見ないため、同じcontrolを `grpc_lite_bench_unary_batch()` へ渡すとinformational fieldを全て無視し `ok=1` になり得る。generic final metadataが既存helper上 `metadata_too_large` を立ててもsuccess gateが無視する点は以前からのdiagnostic simplificationだが、今回新設したwire counterがbenchで一度もactiveにならない事実は変わらない。`docs/SPEC.md:90,247`、`docs/design/http2-transport-design.md:48`、pass 2 record `:43,45` にdiagnosticのbudget exemptionはない。
- Expected model: diagnostic callbackがproductionと同じresponse header phaseを名乗る場合、valid informational / pseudo / final fieldを含むfixed entry work budgetもiteration-localに適用し、上限超過sampleを成功計測へ含めない。意図的にbenchをresource policy対象外とするなら、shared state / docs / success classificationからその境界を明示する。
- Why it matters: bench-enabled buildは通常production surfaceではないため影響は限定されるが、壊れたfixtureやpeerが大量1xxを返すとdefault timeout 0のbatchがproductionに存在する停止点を持たず、production-invalidな応答を正常な性能sampleとして記録する。
- Recommended fix: diagnosticで使う実limitを明示的に初期化し、productionと共有できるaccounting decisionをsemantic early returnより前に呼ぶ。counterと `metadata_too_large` をiterationごとにresetし、超過時はstream-local cancelしてsuccess gateから除外する。既存 `informational-entry-budget` controlをPHPT 043でも実行する。
- Fix summary: benchのnormal / invalid header callbackもproductionと同じ共有accounting ownerをphase early return前に呼ぶようにした。default 64KiB limitを初期化し、wire / semantic counterと`metadata_too_large`をiterationごとにresetし、CANCELをlive sessionへqueueしてover-budget sampleをsuccessから除外した。
- Fix commit: `pending`
- Verification: PHPT 043でentry budgetとdefault 64KiB byte budgetが各`ok=0 / failed=1 / stream_error_code=8`、near-cap valid informational responseの2 iterationsが`ok=2 / failed=0`となることを確認。PHPT 28/28、static analysis PASS。
- Notes: `pass 1 C-safety REVIEW-20260715-001のproduction fixは有効。これはbench diagnosticに限定したparity / resource-boundary findingである。` protocol `REVIEW-20260715-002` / test `REVIEW-20260715-001`と同一defectとして、limit initialization、shared accounting、iteration reset、CANCEL、success classificationを一体で修正した。

### REVIEW-20260715-004: bench outbound RST observerがpushed streamのPROTOCOL_ERRORをcurrent RPCへ誤帰属する

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `C safety / lifetime adversary`
- Finding: `6a4902f` で追加したbench `on_frame_send` observerはsession上の全outbound `RST_STREAM(PROTOCOL_ERROR)` をcurrent batch callのresponse errorとして扱い、frame stream idを確認しない。server-pushed streamに対してnghttp2が自動生成したresetでもmain requestの `response_header_protocol_error` / `discard_response_body` を汚染し、cleanなmain responseをfalse failureにする。
- Evidence: `src/diagnostic/bench.c:455-468` はsession-global user dataの単一 `grpc_call *` を使い、RST type / error code / main callのphaseだけを検査するが、`frame->hd.stream_id == call->stream_id` gateがない。begin/header/invalid/close/frame-recvの新規semantic mutationはそれぞれcurrent stream idをgateする。bench sessionは `src/diagnostic/bench.c:1326` で同じcall pointerをsession user dataにし、SETTINGS `:1332-1338` はproduction `src/transport.c:1751-1758` と異なり `SETTINGS_ENABLE_PUSH=0` を送らないため、別のpromised streamが同一sessionに存在できる。exact probeはopenなrequest stream 1へvalid `PUSH_PROMISE` でstream 2を作り、stream 2へ `HEADERS(:status 103, END_HEADERS|END_STREAM)` を送ってnghttp2に `RST_STREAM(PROTOCOL_ERROR)` を自動queueさせ、stream 1のfinal response前にflushする。その後stream 1へcleanな `:status 200` / application/grpc / DATA / `grpc-status: 0` trailerを返す。observerはstream 2のRST時点でmain callの `final_response_headers_seen == false` を見てprotocol errorを立て、現benchは `ok=0 / failed=1` になり得る。
- Expected model: call-local response error stateは同じHTTP/2 stream idに属するframeだけが更新する。pushed / foreign / closed streamのcontrol frameはcurrent RPC sampleのstatusを変更しない。
- Why it matters: production stateはstream user-data lookupで正しいcallへ帰属するが、benchだけは同一session上のforeign stream eventで成功sampleを失う。benchmark peerやproxyがHTTP/2 pushを使うと、main responseに欠陥がなくても測定が失敗する。
- Recommended fix: bench RST observer predicateへ `frame->hd.stream_id == call->stream_id` を追加する。production parityとしてbenchも `SETTINGS_ENABLE_PUSH=0` を送ると防御が増すが、callback ownershipのstream-id gateは独立して必要である。上記pushed-stream probeでmain call `ok=1 / failed=0` を固定する。
- Fix summary: bench outbound `RST_STREAM(PROTOCOL_ERROR)` observerへ`frame->hd.stream_id == call->stream_id` gateを追加し、foreign stream eventがcurrent RPC stateを更新しないようにした。raw fixtureはpromised streamのPROTOCOL_ERROR RSTを受信するまでmain responseを保留し、その後main streamへclean OKを返す。
- Fix commit: `pending`
- Verification: PHPT 043の`foreign-pushed-stream-protocol-rst`でpromised streamのPROTOCOL_ERROR RST観測後にmain callが`ok=1 / failed=0 / stream_error_code=0`となることを確認。PHPT 28/28 PASS。
- Notes: `productionの on_frame_send_callback() はframe stream idから grpc_call_from_stream_id() を引くため同じ誤帰属はない。` `SETTINGS_ENABLE_PUSH=0`はownership gateの代替にならずexact probeを実行不能にするため追加せず、stream-id gateを採用した。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `2`
- Low: `2`
- Design Decision: `none`
