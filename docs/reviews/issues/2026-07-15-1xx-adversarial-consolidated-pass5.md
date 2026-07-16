# 1xx informational response adversarial consolidated pass 5 2026-07-15

## Scope

- Commits `20c2dc0` / `6a4902f` / `0e22a8a`（current HEAD）
- `src/response_header_phase.[ch]`
- `src/grpc_exchange_state.h`
- `src/transport_core.[ch]`
- `src/status_core.c`
- `src/transport.c`
- `src/diagnostic/bench.c`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `poc/test-server/main.go`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- `tests/unit/test_response_header_phase.c`
- `tests/unit/test_status_core.c`
- `tests/unit/test_transport_core.c`
- pass-1 / pass-3 adversarial review 6 records、pass-2 / pass-4 domain-model gate 2 records
- `docs/issues/open/2026-07-10-informational-1xx-response-handling.md` と関連design / verification docs

## Reviewer Role

- consolidated adversary（HTTP/2 / gRPC protocol + C safety / lifetime + test / fixture）

## Review Prompt Summary

- pass 5 convergence checkとして、pass-3修正のshared wire-budget owner、`TEMPORAL_CALLBACK_FAILURE`と`RESOURCE_EXHAUSTED`のpriority、diagnostic iteration reset、terminal status gate、foreign pushed-stream attribution、PHPT 042 / 043のoracleをcurrent HEADで再監査した。
- malformed 1xx、END_STREAM status commit、call / retry / bench reuse時のphase freshness、normal / invalid header callback、late frameのstream ownershipを静的に追跡した。指示どおりtest suite / Dockerは実行していない。
- issue Decision Logで受容済みのstaging非採用、独立wire budget、pure phase helper、invalid-frame / outbound protocol-RST observerの選択は再議論していない。

## Issues

### REVIEW-20260715-001: terminal status gateがfailure確定後もsilent peerのstreamを終了しない

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `consolidated adversary（protocol + C safety / lifetime + test / fixture）`
- Finding: 103後のnon-END_STREAM `FINAL_INITIAL` blockに `grpc-status` / `grpc-message` / `grpc-status-details-bin` があると、production / diagnosticは `invalid_grpc_status` とbody discardを確定するが、`RST_STREAM`をsubmitしない。peerがそのHEADERS後に何も送らずconnectionを開いたままにすると、deadlineなしのunary、server streaming、raw batchはいずれも既に確定したfailureを返せず無期限に待つ。
- Evidence: productionのstatus field branchは `src/transport.c:2290-2337` でshared END_STREAM predicateの失敗をflagへ写すだけでreturnし、frame-endの `src/transport.c:2494-2505` も同じflagを再確認するだけである。unaryは `src/unary_call.c:201-247` の `while (!call.stream_closed)`、server streamingは `src/server_streaming_call.c:268-340` のloop / cancel predicateから `invalid_grpc_status` が抜けている。diagnosticも `src/diagnostic/bench.c:392-436,604-611` でflagだけを立て、default `timeout_us=0` のloop `:1651-1673` はstream closeを待つ。現fixtureは `poc/test-server/main.go:805-823` でinvalid block直後にDATA END_STREAMまたはDATA + terminal trailersを必ず送り、PHPT 043 `:34-40` はその結果の `failed=1` だけを見るためsilent peerを再現しない。`docs/guides/code-reading-guide.md:237` はinvalid grpc-statusをstream-local failureへ変換して該当streamへ `RST_STREAM` を送ると説明するが、このpathでは送られない。
- Expected model: END_STREAMなし `FINAL_INITIAL` でstatus fieldを観測した時点でresponseは回復不能なgRPC protocol failureである。`invalid_grpc_status` の `UNKNOWN` taxonomyを保持したまま対象streamをpromptにcancelし、connectionはusableなら再利用する。failure確定後にpeerの追加DATAまたはstream closeを待たない。
- Why it matters: deadlineを設定しない通常callやbenchmarkを、malformed peer / proxyが1つのHEADERS blockだけで永久に占有できる。DATAを送り続ける場合もbodyはdiscardされるだけでterminal actionがなく、PHP workerとsocket readをpeer側の都合で継続させる。memory corruptionではないが、fixが約束するmalformed sample rejection / status返却がpeerの協力に依存する。
- Recommended fix: production / diagnostic双方のvalid `FINAL_INITIAL` frame-endで `initial_grpc_status_seen && !initial_headers_end_stream` を検出したら、flag確定に加えてmain streamへ `RST_STREAM(CANCEL)` を一度submitする。server streamingのloopを抜けるだけではopen streamのuser-data lifetimeを残すため、必ずtransport actionを伴わせる。raw fixtureへ `103 -> non-END_STREAM FINAL_INITIAL(status field) -> silence until exact CANCEL` controlを追加し、少なくともpass-3で漏れた `grpc-status-details-bin`、可能なら3 status fieldすべてを固定する。
- Fix summary: production / diagnosticが共有するframe-end helperを追加し、validなEND_STREAMなし`FINAL_INITIAL` blockでstatus fieldを観測した場合に`invalid_grpc_status`を確定してmain streamへ`RST_STREAM(CANCEL)`をsubmitするようにした。`grpc-status` / `grpc-message` / `grpc-status-details-bin`のsilent controlsを追加し、2秒guard、`UNKNOWN / invalid grpc-status trailer`、exact CANCEL、同一connection follow-upを固定した。diagnosticはfailed-not-timedoutとerror code 8を確認する。
- Fix commit: `pending`
- Verification: `PHPT 042で3 status fieldそれぞれのunary / server streamingが2秒guardより前にUNKNOWN + invalid grpc-status trailerを返し、fixtureが対象streamのexact RST_STREAM(CANCEL)を観測した同一connection follow-upだけがOKとなることを確認。PHPT 043で3 fieldともok=0 / failed=1 / timed_out=false / stream_error_code=8を確認。./tools/test/check-phpt.sh 28/28 PASS。`
- Notes: pass-3 protocol `docs/reviews/issues/2026-07-15-1xx-adversarial-protocol-pass3.md` の `REVIEW-20260715-003`（およびその起点のpass-1同ID）に対するterminal-gate fixの不足として新規採番した。direct-final responseにも類似する既存liveness gapはあるが、本指摘は `0e22a8a` が追加したpost-1xx `grpc-status-details-bin` gateとPHPT oracleを含む対象scopeに限定する。

### REVIEW-20260715-002: pre-final wire-budget超過のRESOURCE_EXHAUSTED detailsがHTTP status未受信を報告する

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `consolidated adversary（protocol + C safety / lifetime + test / fixture）`
- Finding: informational HEADERSだけでwire budgetを超えたcallはstatus codeを正しく `RESOURCE_EXHAUSTED` とする一方、PHP-visible detailsは `HTTP status -1 without grpc-status` になる。primary failureはlocal header resource limitであり、final HTTP status未受信はその結果なので、codeとdetailsのtaxonomyが矛盾している。
- Evidence: `src/transport.c:2216-2241` はfinal `:status` 前でも `metadata_too_large` を立ててCANCELをqueueし、informational `:status` はsemantic isolationにより `http_status == -1` のままである。`src/status_core.c:18-38` はこのshapeを意図して `metadata_too_large` をHTTP fallbackより先に評価する。しかしdetails builderは `src/transport.c:2589-2628` でgeneric `http_status != 200` branchを `RESOURCE_EXHAUSTED` branchより先に通すため、resource-limit reasonへ到達しない。PHPT 042のresource helper `tests/phpt/042-informational-1xx-adversarial.phpt:101-149` はresponse / code / exact CANCEL後のfollow-upだけをassertし、detailsを見ない。`docs/design/protocol-classification-boundary.md:37` もfinal HTTP status前の `metadata_too_large` priorityを明示する。
- Expected model: status detailsは `grpc_lite_status_code_from_call()` が選んだprimary outcomeと同じfailure ownerを説明する。pre-final / post-finalのどちらでmetadataまたはwire-header上限に達しても、HTTP fallbackやmessage-sizeではなくresponse header / metadata budget超過を示す。
- Why it matters: codeによるretry判断は正しいが、log / telemetry / operator diagnosisは存在しないHTTP responseの欠落へ誘導される。metadata limitの設定値やhostile 1xx列を調べるべき事象をupstream HTTP availability failureと誤認させる。
- Recommended fix: `grpc_lite_status_details_from_call()` のgeneric HTTP fallbackより前に、`code == GRPC_STATUS_RESOURCE_EXHAUSTED && call->metadata_too_large` の専用details branchを置く。message-size / read-ahead limitとは異なるheader-budget用文言にする。fixtureはfinal OKを送らずbudget超過後のexact CANCELを待つcontrolにすると、pre-final priorityを決定的に固定できる。
- Fix summary: `grpc_lite_status_details_from_call()`で、status codeが`RESOURCE_EXHAUSTED`かつ`metadata_too_large`の場合の専用detailsをresponse-header protocol errorの直後、generic HTTP / grpc-message / I/O fallbackより前に配置した。pre-final entry / byte / invalid-header budget controlsはoverflow後にsilentとなり、unary / server streamingで専用detailsとexact CANCEL / connection reuseを固定した。
- Fix commit: `pending`
- Verification: `PHPT 042のpre-final informational entry / byte、invalid regular entry / byte各controlでunary / server streamingがRESOURCE_EXHAUSTED + response header/metadata budget exceededを返し、exact RST_STREAM(CANCEL)後のsame-connection follow-upがOKとなることを確認。./tools/test/check-phpt.sh 28/28 PASS。`
- Notes: HTTP 200後の通常metadata overflowが従来からgeneric message-size detailsへfall throughする問題全体は本指摘へ含めない。対象commitが新設した「final status前でもwire budgetを優先する」pathで、明示的にHTTP fallbackと矛盾する部分だけを扱う。

### REVIEW-20260715-003: invalid-header budgetのPHPTがdiagnostic callbackとTEMPORAL stopを識別しない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `consolidated adversary（protocol + C safety / lifetime + test / fixture）`
- Finding: pass-4 `REVIEW-20260715-001` で修正したinvalid-header callbackの `NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE` propagationと、pass-3で追加したdiagnostic invalid-header accountingには、それぞれ修正前へ戻した場合に失敗するoracleがない。current sourceは正しいが、PHPT 042 / 043はこの2つのcallback wiring / stop semanticsを回帰gateにできていない。
- Evidence: production / diagnosticのcurrent callbacksは `src/transport.c:2366-2381` と `src/diagnostic/bench.c:476-486` でshared ownerのreturnを伝播する。しかしproduction fixtureのinvalid entry control `poc/test-server/main.go:763-771` は`:status` + invalid field 128個なのでoverflow fieldがblockの最後であり、invalid byte control `:772-778` もoversized fieldが1個だけである。TEMPORALをpass-4修正前のように0へ変換しても、その後に観測可能なcallbackはなく、既に立った `metadata_too_large`、queue済みCANCEL、RESOURCE_EXHAUSTED、follow-upはすべて同じになる。さらにPHPT 043 `tests/phpt/043-informational-1xx-bench-parity.phpt:42-50` はvalid regular fieldのentry / default-byte controlsしか実行しないため、bench invalid-header callbackをno-opにしても全assertionが成立する。C unitはpure arithmetic helperだけを検証する。pass-4 record `docs/reviews/issues/2026-07-15-1xx-pass3-fix-domain-model-pass4.md:34,37-40` 自身も既存fixtureがsame-block cutoffを観測しないと記録し、focused oracleを推奨している。
- Expected model: pass-3 / pass-4で修正したresource boundaryは、normal / invalid callbackとproduction / diagnosticの各consumerで回帰を識別できる。budget超過fieldでcallback processingが止まり、block内の後続invalid fieldへapplication callbackを継続しないことまでgateする。
- Why it matters: 将来のcallback登録整理やnghttp2 error handling変更で、diagnosticだけinvalid fieldを無計上に戻す、またはproduction / diagnosticがCANCELをqueueしながらblock残部のcallback workを続ける回帰が入っても、現在の必須gateはgreenのままになる。今回のhard resource boundaryで最も壊れやすいorchestration部分がpure helper coverageの外に残る。
- Recommended fix: raw controlを`:status: 103` + NUL-bearing invalid regular field 129個へし、accounting前にincrementするbench/test-only `invalid_header_callback_count`（productionにも同等のtrace / fault seam）を観測する。正しい実装は`:status`で1 entry、最初の127 invalid fieldで上限128、128個目のinvalid callbackでoverflowしてTEMPORALを返すためcallback count 128で止まる。TEMPORAL握り潰しは129、callback未登録は0となる。PHPT 043で `failed=1`、`stream_error_code=8`、callback count 128をassertし、production側も同じcutoffを識別するoracleを追加する。
- Fix summary: invalid entry controlを`:status: 103` + NUL-bearing invalid regular field 129個へ変更した。productionはvalue-free `wire.response_invalid_header` trace、diagnosticはiteration-local `invalid_header_callback_count`でcallback entryを観測し、両方ともoverflowする128回目でTEMPORALが伝播して129個目を処理しないことをPHPTで固定した。
- Fix commit: `pending`
- Verification: `productionがTEMPORALを0へ変換するmutationではPHPT 042がexpected 128 / got 129で失敗した。diagnostic invalid-header callbackをno-opにするmutationではsilent fixtureへCANCELを送れず、外側10秒guardがexit 124となった。両mutationをrestore後、focused PHPT 042 / 043は2/2 PASS、full ./tools/test/check-phpt.shは28/28 PASS。`
- Notes: active verification docsにsame-block callback cutoffをruntime-coveredとする明示的な過大主張は見つからなかったため、docs defectは別countしない。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
