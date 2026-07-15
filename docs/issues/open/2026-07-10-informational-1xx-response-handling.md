# 1xx (informational) 応答の HEADERS を final response として誤処理しない

- Status: Open
- Created: 2026-07-10
- Branch: codex/issue-informational-1xx-response-handling
- Owner: Claude

## Background

[2026-07-08-status-taxonomy-official-alignment](2026-07-08-status-taxonomy-official-alignment.md)（PR #28）の敵対的再レビュー [Medium] 指摘（`NGHTTP2_HCAT_HEADERS` ≠ terminal trailers）への対応中に一度実装したが、第三パスレビュー（protocol-adversary `REVIEW-20260710-004`）で「frame-end 判定だけの不完全な 1xx 成功経路」と指摘され、PR #28 からは revert して本 issue の別 PR スコープとした。

nghttp2 の category 契約では、最初の response HEADERS だけが `NGHTTP2_HCAT_RESPONSE` で、1xx (informational) の場合は後続の non-final block と final response HEADERS がすべて `NGHTTP2_HCAT_HEADERS` で届く。着手前の実装は:

1. `HCAT_RESPONSE`（= 1xx block）に対して content-type validation を実行するため、content-type を持たない 1xx で `invalid_content_type` が誤発火し、1xx を挟む応答は失敗する（既知の制限として許容中）。
2. `on_header_callback()` は raw category だけで trailing / initial を決めて即時に call state へ反映するため、frame 完了後にしか分からない「この block は 1xx / final / trailing のどれか」という semantic phase を知らない。

## 却下された初回実装（参考）

PR #28 の commit `375c3dd` で `expect_final_response` フラグによる frame-end 判定を実装したが、以下の理由で revert（レビュー実測 probe 付き）:

- 1xx 後の final response HEADERS のフィールドが trailing metadata として保存される（`x-bench-observe-authority` + 103 併用で `x-bench-authority` が `getMetadata()` から `getTrailingMetadata()` へ移動）。
- 1xx block 内の `content-type` / `grpc-status` / `grpc-message` / `grpc-encoding` が final response の validation / status / compression 分類を汚染しうる（RFC 8297 §2: 103 のフィールドは final response の処理に影響してはならない）。
- frame-end の `on_frame_recv_callback()` では、既に行われた header callback の metadata 追加や semantic field 更新を修復できない。

## Goals

- response header block に call-local な semantic phase（informational / final initial / trailing）を持たせ、`on_begin_headers_callback()` 等で **header callback 時点までに** phase を確定する。
- informational block のフィールドを隔離する（metadata / validation / status state に反映しない）。
- 1xx 後の final response `HCAT_HEADERS` を initial response headers として処理する（metadata は initial 側、validation も initial 相当）。
- unary / server streaming で status / details に加えて **metadata ownership（initial / trailing の帰属）** と 1xx field isolation をテストで固定する。

## Non-Goals

- 100-continue 等、1xx への能動的な応答動作。受動的に無視して final response を待つのみ。

## Plan

- `on_begin_headers_callback()`（または frame header 到達時点）で block phase を決定: 未 final なら `:status` を見る前に informational 候補として開始し、`:status` 確定時に phase を確定する方式を検討（`:status` は block 先頭で届く保証があるため header callback 内での確定も可）。
- fixture: `x-bench-early-hints=1`（103 先行送出、他 control と併用可、PR #28 から revert したものを復活）に加え、1xx block に semantic field（content-type / grpc-status 等）を含める汚染ケースを追加。
- PHPT: 1xx + no-trailers → INTERNAL / 1xx + status0 → OK / metadata ownership（`x-bench-observe-authority` 併用で initial 帰属を assert）/ 1xx field isolation を unary / streaming で固定。

## Progress

- 2026-07-10: PR #28 内で `expect_final_response` による frame-end 判定を一度実装（`375c3dd`）→ 第三パスレビュー `REVIEW-20260710-004` の指摘（上記「却下された初回実装」）を受けて PR #28 から revert。terminal frame 判別（`trailing_headers_seen` の END_STREAM ゲート）のみ PR #28 に残した。
- 2026-07-15: `grpc_call` に response header-block semantic phase（`AWAITING_STATUS` / `INFORMATIONAL` / `FINAL_INITIAL` / `TRAILING`）と final response 観測stateを追加した。`on_begin_headers_callback()` でblock開始phaseを決め、先頭の`:status` callbackで informational / final initial を確定する。1xx fieldはmetadata count/bytesを含むcall stateへ反映せず、1xx後に `HCAT_HEADERS` で届くfinal responseへinitial metadata ownershipとinitial content-type validationを適用する。
- 2026-07-15: `x-bench-early-hints=1` fixtureを復活し、103だけにinvalid `content-type`、`grpc-status`、`grpc-message`、`grpc-encoding`、custom metadataを載せる `x-bench-early-hints-pollution=1` を追加した。PHPT 022でunary / server streaming双方のmissing trailers、status 0、metadata ownership、informational field isolationを固定した。
- 2026-07-15: bench-enabled diagnostic nghttp2 callbackにも同じphase transitionと反復callごとのstate resetを適用し、production / diagnosticのどちらでもinformational fieldがmetadata / status観測へ混ざらないようにした。
- 2026-07-15: `docs/SPEC.md`、response exchange / transport / protocol classification設計、code-reading guide、fixture / verification資料をsemantic phase modelへ更新した。
- 2026-07-15: HTTP/2 / gRPC domain model reviewを実施し、Blocker / High / Medium / Low / Design Decisionの指摘はいずれもnone。記録は `docs/reviews/issues/2026-07-15-informational-1xx-response-domain-model-review.md`。
- 2026-07-15: pass-1 adversarial review 8件を受け、response phaseとstatus commit validityのproduction / diagnostic共有pure helper、`response_header_protocol_error` taxonomy、END_STREAMなしtrailing blockのquarantine、nghttp2 invalid-frame / outbound protocol-RST観測を追加した。HTTP messaging rejectionは先行 `grpc-status: 0` とHTTP status未観測fallbackより優先し `INTERNAL` とする。
- 2026-07-15: semantic metadata ownershipとwire header work budgetを分離し、pseudo-headerとinformational field、複数1xx blockをoverflow-safeなcall-local entry/byte counterへ累積するようにした。budget超過は `RESOURCE_EXHAUSTED` + stream-local cancelとし、同一connectionの後続RPC成功もfixtureで固定した。
- 2026-07-15: raw h2c fixture `:50071` とPHPT 042/043を追加。END_STREAMなしtrailer、103+END_STREAM、103→missing-status HEADERS、103→DATA、informational entry/byte budget、invalid status前後のmetadata ownership、bench false-successをunary / server streamingまたはbench entrypointで固定した。PHPT 022には複数103、post-1xx Trailers-Only / invalid content-type、two-message streamingを追加し、request cardinalityを1に揃えた。
- 2026-07-15: pass-3 adversarial reviewの重複12指摘を6テーマへ整理し、invalid regular header callbackを含むwire header budget、diagnostic default limit / iteration reset / success gate、`grpc-status-details-bin` terminal gate、foreign pushed-stream RST ownershipを修正した。wire budget算術は`transport_core`のpure helper、call classificationと`RST_STREAM(CANCEL)`はproduction / diagnostic共有ownerへ置き、semantic phase責務と分離した。
- 2026-07-15: raw fixtureのresource oracleを対象streamのexact `RST_STREAM(CANCEL)`受信でgateし、pseudo / regular field classを片側実装では通せない65-block / 237-byte境界へ変更した。silent-ignoreされるNUL-bearing invalid regular field、diagnostic default 64KiB、non-terminal status-details、2-iteration reset、foreign pushed-streamのcontrolsを追加した。
- 2026-07-15: PHPT 043のvalid positive controlにより、raw benchが`call.connection == NULL`のままproduction `connection_send()`を呼び、全batchを送信前に失敗させていた既存不整合を検出した。raw diagnosticのsocket ownershipに合わせてbench-local fd sendへ修正し、positive 2 iterationsとnegative controlsの双方を通した。
- 2026-07-15: pass-4 domain model gateで、invalid-header budget超過後のTEMPORAL callback stopを0へ変換していたMedium 1件を検出した。production / diagnostic双方でTEMPORALを伝播し、invalid-frame observerは既確定の`metadata_too_large`をprotocol errorで上書きしないよう修正した。再レビュー後のBlocker / High / Medium / Low / Design Decisionはすべてnone。
- 2026-07-15: consolidated pass-5のMedium 1件を受け、END_STREAMなし`FINAL_INITIAL`でstatus fieldを観測したvalid blockのframe-endに、production / diagnostic共有helperからmain streamへ`RST_STREAM(CANCEL)`をsubmitするようにした。`grpc-status` / `grpc-message` / `grpc-status-details-bin`のいずれでも`UNKNOWN` taxonomyを維持したままsilent peerを待たずcallを終了し、同一connectionを再利用する。
- 2026-07-15: pass-5のLow 2件に対し、pre-final wire-header budget超過のdetailsを`response header/metadata budget exceeded`へ揃えた。invalid regular fieldを129個送るcontrolと、productionのvalue-free trace / diagnosticのiteration-local callback countを追加し、128回目のoverflowでTEMPORALが伝播して129個目を処理しないruntime oracleを固定した。
- 2026-07-15: consolidated pass-7のLow 1件を受け、terminal status-field gateのCANCEL flushが失敗しても、public status code / detailsのprimary ownerを`invalid_grpc_status`へ揃えた。secondaryなI/O failureはcall / connection diagnosticに保持し、test-onlyのpost-nghttp2 / pre-socket-flush EPIPE seamとPHPT 044で3 status fieldのunary / server streaming、dead connection eviction、fresh follow-upを固定した。
- 2026-07-15: pass-7のPHPT liveness Low 1件に対し、PHPT 043で`timeout_us > 0`のbatchだけ`poll_loop=true`とした。silent status-field / wire-header budgetの回帰時はdeadline-aware poll経路が約2秒で`timed_out=true`を返し、runner全体のtimeoutではなく既存assertionで失敗する。timeoutなしのbench baselineはblocking経路のままとし、blocking branch自体のdeadline挙動は変更していない。
- 2026-07-15: consolidated pass-9のMedium 1件を受け、END_STREAMなし`FINAL_INITIAL`のstatus fieldをframe-endではなくfield callback時点で`UNKNOWN` + `RST_STREAM(CANCEL)`へ確定した。END_HEADERS未完了blockはconnectionをdrainingとしてquarantineし、production / diagnosticの3 status field、unary / server streaming、fresh follow-upをraw fixtureで固定した。
- 2026-07-15: pass-9 fixのdomain-model pass-10で、END_HEADERS未完了のinbound HPACK blockをdrainingに留めると同一connection上の既存siblingがI/Oを継続して停止し得るMedium 1件を確認した。connection-localなclose-after-pending-flush stateを追加し、対象streamのCANCELをflushした後にconnectionをdead化する。unary / server streamingのdrive loopはdead後にsession / socketを再駆動せず、対象callは`UNKNOWN`、既存siblingは`UNAVAILABLE`へ有限に収束する。raw fixture / PHPT 042へactive server-streaming sibling、別streamのincomplete status block、exact CANCEL、siblingの追加wire I/Oなし、fresh follow-upを識別するmultiplex probeを追加した。
- 2026-07-15: pass-9のLow 1件に対し、END_STREAM付き`FINAL_INITIAL`で`grpc-status` / `grpc-message` / `grpc-status-details-bin`のいずれかを観測した時にblock-local Trailers-Only candidateへ遷移し、先行metadataをtrailingへ移して後続も同じownershipへ揃えた。message-only / details-only blockのbefore-after metadataをunary / server streamingで固定した。
- 2026-07-15: consolidated pass-11のMedium 1件に対し、terminal quarantine専用flushをcall deadlineと独立した固定50ms graceへ分離し、quarantine開始後のDATA providerをdeferしてsibling application DATAを新たにdriveしないようにした。1024-byte stream window、16MiB sibling、target直前のWINDOW_UPDATE、750ms peer read stallを組み合わせ、deadlineなしtargetの有限`UNKNOWN`、siblingの`UNAVAILABLE`、追加DATA不在をPHPT 042で固定した。
- 2026-07-15: pass-11のLow 2件に対し、END_HEADERS未完了targetの`RST_STREAM(CANCEL)`をfixtureのpeer受信markerでgateし、traceをattribution補助へ限定した。bench diagnosticにはentry resetと独立した約32.8KiB/iterationのwire byte-counter reset controlを追加した。初回PHPTでfixture setupがconnection flow-controlの循環待ちになることを検出したため、trigger前だけheld siblingの受信DATAをconnection-levelへ返却し、sibling stream windowを枯渇させたままtarget requestを完了可能にした。
- 2026-07-15: consolidated pass-13のMedium 1件に対し、END_HEADERS未完了blockのterminal quarantineをstatus field専用経路から共有incomplete-block actionへ拡張した。informational END_STREAM、END_STREAMなしtrailing、normal / invalid header callbackのwire budget超過もprimary taxonomyとcaller-selected target RST codeを維持したままconnection terminal化し、production / raw diagnosticが`response_header_block_incomplete` classificationを共有する。3種のfragmented controlとunary / server streaming / diagnostic probeを追加した。
- 2026-07-15: pass-13のLow 2件に対し、multiplex fixtureのsibling DATA違反境界をpeerがtarget CANCELを受信した後へ移した。32MiB WINDOW_UPDATEとbudget overflow HEADERSを50ms分割し、pre-target-RST DATAをpositive setupとして必須にしつつ、client trace / peer markerの双方でpost-target-RST DATAを拒否する。terminal graceは合法なpre-quarantine backpressureを含むend-to-end wall timeではなく、client traceのtarget RSTから`rpc.end`まで500ms未満で固定した。ownership map、raw fixture catalog、verification matrixにはcall-local incomplete stateのproducer / consumer / resetと、authority-keyed follow-up markerの最大3秒wait / consume semanticsを反映した。
- 2026-07-15: consolidated pass-15のMedium 1件に対し、wire budget計上後の`AWAITING_STATUS`でnormal / invalid regular fieldを観測した時点をshared protocol classificationへ追加した。後続`:status`を合法に置けないため`response_header_protocol_error`を確定し、END_HEADERS未完了時は既存incomplete-header terminal actionへ`PROTOCOL_ERROR`を渡してproduction connection quarantineとdiagnostic finite finishを共有する。valid / NUL-bearing invalid regular fieldのfragmented controlsをunary / server streaming / diagnosticで固定した。
- 2026-07-15: consolidated pass-15のLow 1件に対し、NUL-bearing invalid field 129個でEND_HEADERS未完了のentry budgetを超えるcontrolを追加した。productionは`RESOURCE_EXHAUSTED`、exact CANCEL、terminal connection、fresh follow-upを、diagnosticはcallback cutoff 128とdefault-blocking batch後の実fd `O_NONBLOCK` stateを確認し、invalid producerとnonblocking consumerを別々のmutationで識別した。
- 2026-07-15: pass-15対応に合わせ、SPEC、exchange-state / protocol-classification design、code-reading guide、fixture catalog、verification matrix、compatibility checklistをcurrent modelへ更新した。HTTP/2 / gRPC domain model reviewはBlocker / High / Medium / Low / Design Decisionすべてnone。
- 2026-07-15: consolidated pass-17のMedium 1件に対し、invalid-header callbackのempty nameをregular fieldとして扱うpure predicateを`transport_core`へ追加した。wire budget先行順序を維持したまま既存のregular-before-`:status` rejectionとshared incomplete-header terminal actionへ接続し、productionは`INTERNAL` + `RST_STREAM(PROTOCOL_ERROR)` + terminal connectionへ、diagnosticはfailed-not-timedout + exact RST + nonblocking finite finishへ収束させる。
- 2026-07-15: 完了した103の後にexact HPACK field `00 00 01 76`を持つEND_STREAM / END_HEADERSなしHEADERSを送り、CONTINUATIONを省くraw controlを追加した。PHPT 042でunary / server streamingのstatus・details・exact RST・fresh follow-upを、PHPT 043でfailed-not-timedout・invalid callback 1回・実fd `O_NONBLOCK`を固定し、pure name predicateをC unitへ追加した。
- 2026-07-15: consolidated pass-19のMedium 1件に対し、response field producerを`STATUS` / `REGULAR` / `INVALID_REGULAR` / `REJECTED`へ分類し、`NONE` / `AWAITING_STATUS` / `INFORMATIONAL` / `FINAL_INITIAL` / `TRAILING`とのclosed route tableへ接続した。normal / invalid callbackはshared wire budgetを先に通し、callbackを迂回するstrict rejectionとblock-end rejectionは`REJECTED` default routeからshared incomplete-header terminal actionへ入る。RST submit ownershipとconnection lifecycle markを分離し、nghttp2-owned `PROTOCOL_ERROR`を重複submitしない。
- 2026-07-15: production / diagnosticへ同じfield classifier / route / incomplete lifecycle markを適用し、strict-invalid pseudo-header `:foo: v`とuppercase regular name `X-Bad: v`のexact HPACK controlsを追加した。C unitはdeclared / unknown field class × 全5 phaseをtable-driveし、PHPT 042は両controlのunary / server streaming、PHPT 043はstrict callback bypassと実fd nonblocking finite finishを固定した。owning design docにはbudget visibility、全route、primary taxonomy、RST ownerを含むexhaustive tableを追加した。
- 2026-07-15: 初回の全PHPTでinvalid regular fieldのincomplete budget caseが`RESOURCE_EXHAUSTED`から`INTERNAL`へ退行することを検出した。nghttp2はこのinvalid-header stopをinvalid-frame observerへ`HTTP_HEADER`として通知するため、producer-owned分岐は`metadata_too_large`またはlocal TEMPORALの双方を条件とし、budget priorityとgeneral field-route priorityを両立させた。
- 2026-07-15: pass-21のlate incomplete HEADERS findingを調査し、valid terminal responseでstreamがclose / unregisterされた後はfield callbackとcall-local field-class × phase routeのproducer集合から外れることを確認した。ignored closed / foreign streamのHEADERS frame headerをconnection/session scopeで観測し、END_HEADERS未完了なら完了済みcallのtaxonomyやstream RSTへ触れず、新規admission停止と有限なconnection teardownだけを指示する方針とした。
- 2026-07-15: pass-21対応としてproduction / diagnosticの`on_begin_frame`をshared pure predicateへ接続した。productionはlive callを持たないincomplete HEADERSをcall pointerなしで既存connection quarantineへ写像し、diagnosticはiteration reset外のsticky session markerから次requestをsubmit前に停止する。same-write raw fixtureとPHPT 042 / 043でunary / server streaming先行callのOK不変、RST誤帰属なし、fresh follow-up、diagnosticの1件目OK / 2件目未submitを固定した。
- 2026-07-16: pass-21 fixのroutine QA finalizationとしてcurrent working treeをfindingと再照合し、実装、fixture、tests、design / verification資料、review記録に未完了やstaleな記述がないことを確認した。コードの追加修正は行わず、test-server rebuild / recreateと4 suiteの再検証のみ実施した。
- 2026-07-16: consolidated pass-23のMedium 1件を受け、live callが所有するEND_HEADERS未完了response blockをdeadline / explicit cancel / destructor / error teardownで放棄する遷移を再監査した。block開始時はpass-21のconnection-scope predicateから意図的に除外される一方、既存のcall teardownは`response_header_phase.block_phase`をconnection lifecycleへhandoffせず、stream unregister後もpoisoned HPACK decoderをpersistent reuseへ残し得ることを確認した。
- 2026-07-16: pass-23対応として`block_phase != NONE`をopen inbound response blockへ写像するshared pure predicateを追加し、live local abandonmentが収束する`cancel_grpc_call_stream()`のRST submit前で既存incomplete-header connection terminal actionへhandoffした。complete rejected HEADERSはinvalid-frame fallbackでphaseを`NONE`へ戻し、production / diagnosticのfalse positiveを防いだ。raw diagnosticは同じpredicateからsticky session-terminal marker、deadline時のexact CANCEL、nonblocking finite finishへ収束させた。
- 2026-07-16: pass-23 finalizationでfindingとworking-tree diffを再照合し、unary deadline、server-streaming explicit cancel、resource destructor、semantic-error teardownが共通seamへ収束すること、fatal I/O / nghttp2 errorは先にconnectionをdead化すること、PHPT 042 / 043とC unitの識別力を確認した。実装の再作成は行わず、staleだった`cancel_grpc_call_stream()`のconnection reuse commentだけをcurrent exception modelへ揃え、review recordとissue bookkeepingを完了した。

## Verification

- 2026-07-15: `docker compose build test-server` / `docker compose up -d --force-recreate test-server` PASS。wire probeでpollution fieldを持つ103の後に、cleanなfinal 200 initial HEADERSと `grpc-status: 0` trailerが届くことを確認。
- 2026-07-15: `./tools/test/check-phpt.sh` PASS（26/26 tests、failed 0、skipped 0、warned 0）。PHPT 022のunary / server streaming 1xx status、metadata ownership、field isolationを含む。
- 2026-07-15: `./tools/test/check-c-unit.sh` PASS（protocol_core / status_core / transport_core、3/3 suites）。
- 2026-07-15: `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions、failures 0、errors 0、skipped 0）。
- 2026-07-15: `./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled cppcheck、findings none）。
- 2026-07-15: pass-4 HTTP/2 / gRPC domain model review PASS（初回Medium 1件を修正後、Blocker / High / Medium / Low / Design Decision: none）。記録は `docs/reviews/issues/2026-07-15-1xx-pass3-fix-domain-model-pass4.md`。
- 2026-07-15: domain model review PASS（Blocker / High / Medium / Low / Design Decision: none）。
- 2026-07-15: pass-1 adversarial fix後にtest-server imageをrebuildし `docker compose up -d --force-recreate test-server` PASS。raw `:50071` のexact malformed/resource/ownership/bench sequenceと、`:50054` のvalid repeated-103 / post-1xx edgeを送出した。
- 2026-07-15: `./tools/test/check-phpt.sh` PASS（28/28 tests、failed 0、skipped 0、warned 0）。PHPT 042がunary / server streamingのmalformed sequence、wire header budgetとsame-connection follow-up、invalid-status metadata ownership、PHPT 043がbench `ok=0` / `failed=1` を含む。
- 2026-07-15: `./tools/test/check-c-unit.sh` PASS（protocol_core / response_header_phase / status_core / transport_core、4/4 suites）。response phase transition、call reset、status commit / metadata role truth tableを含む。
- 2026-07-15: `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions）。
- 2026-07-15: `./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled cppcheck、findings none）。
- 2026-07-15: pass-2 HTTP/2 / gRPC domain model review PASS（Blocker / High / Medium / Low / Design Decision: none）。記録は `docs/reviews/issues/2026-07-15-1xx-fix-domain-model-pass2.md`。
- 2026-07-15: pass-3 fix後に `docker compose build test-server` と `docker compose up -d --force-recreate test-server` を実行し、raw fixture imageのrebuild / restart PASS。
- 2026-07-15: `./tools/test/check-phpt.sh` PASS（28/28 tests、failed 0、skipped 0、warned 0）。PHPT 042でvalid / invalid regular fieldのentry・byte budget、field-class境界、exact CANCEL、same-connection reuse、status-details gateを、PHPT 043でvalid 2-iteration reset、entry / default-byte budget、status-details、foreign pushed-stream ownershipを確認した。
- 2026-07-15: `./tools/test/check-c-unit.sh` PASS（protocol_core / response_header_phase / status_core / transport_core、4/4 suites）。wire header budgetの128-entry境界、exact byte上限、加算overflowを含む。
- 2026-07-15: `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions）。
- 2026-07-15: `./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled cppcheck、findings none）。
- 2026-07-15: pass-5 fix後に`docker compose build test-server`と`docker compose up -d --force-recreate test-server`を実行し、silent status-field / resource controlsとinvalid-header 129-field controlを含むraw fixture imageのrebuild / restart PASS。
- 2026-07-15: `./tools/test/check-phpt.sh` PASS（28/28 tests、failed 0、skipped 0、warned 0）。PHPT 042で3 status fieldのsilent-peer finite guard、UNKNOWN details、exact CANCEL / same-connection follow-up、pre-final resource details、production callback cutoff 128を、PHPT 043でfailed-not-timedout / CANCELとdiagnostic callback cutoff 128を確認した。
- 2026-07-15: `./tools/test/check-c-unit.sh` PASS（protocol_core / response_header_phase / status_core / transport_core、4/4 suites）。
- 2026-07-15: `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions）。
- 2026-07-15: `./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled cppcheck、findings none）。
- 2026-07-15: invalid-header callback cutoffをmutation検証した。productionでTEMPORALを0へ変換するとPHPT 042が128対129で失敗し、diagnostic callbackをno-opにするとsilent fixtureへCANCELを送れず外側10秒guardがexit 124となった。restore後のfocused PHPT 042 / 043は2/2 PASS。
- 2026-07-15: pass-6 HTTP/2 / gRPC domain model review PASS（Blocker / High / Medium / Low / Design Decision: none）。記録は`docs/reviews/issues/2026-07-15-1xx-pass5-fix-domain-model-pass6.md`。
- 2026-07-15: pass-7 fix後の`./tools/test/check-phpt.sh` PASS（29/29 tests、failed 0、skipped 0、warned 0）。PHPT 044でqueue済みCANCELのflush EPIPE後も3 status fieldのunary / server streamingが`UNKNOWN + invalid grpc-status trailer`を維持し、12 connection prefaceでdead connection eviction / fresh follow-upを確認した。PHPT 043の期限付きcontrolsはdeadline-aware poll経路でfailed-not-timedout / exact CANCELを維持した。
- 2026-07-15: 意図的に応答しないTCP peerへPHPT 043と同じ`poll_loop=true / timeout_us=2_000_000` entrypointを実行し、約2.007秒で`failed=1 / timed_out=true`へ収束することを確認した。
- 2026-07-15: `./tools/test/check-c-unit.sh` PASS（protocol_core / response_header_phase / status_core / transport_core、4/4 suites）。
- 2026-07-15: `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions）。
- 2026-07-15: `./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled cppcheck、findings none）。
- 2026-07-15: pass-8 HTTP/2 / gRPC domain model review PASS（Blocker / High / Medium / Low / Design Decision: none）。記録は`docs/reviews/issues/2026-07-15-1xx-pass7-fix-domain-model-pass8.md`。
- 2026-07-15: pass-9 / pass-10 fix後に`docker compose build test-server`と`docker compose up -d --force-recreate test-server`を実行し、CONTINUATION欠落、multiplex sibling、terminal message-only / details-only controlsを含むraw fixture imageのrebuild / restart PASS。
- 2026-07-15: `./tools/test/check-phpt.sh` PASS（29/29 tests、failed 0、skipped 0、warned 0）。PHPT 042で3 status fieldのEND_HEADERS未完了unary / server streaming、target streamのexact CANCEL、connection terminal化、既存siblingの追加wire I/Oなし`UNAVAILABLE`、fresh follow-up、message-only / details-only blockのmetadata ownershipを、PHPT 043でproduction / diagnostic共有actionのfailed-not-timedout / CANCELを確認した。
- 2026-07-15: `./tools/test/check-c-unit.sh` PASS（protocol_core / response_header_phase / status_core / transport_core、4/4 suites）。Trailers-Only candidateのone-shot transition、block end / call reset、metadata role predicateを含む。
- 2026-07-15: `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions）。
- 2026-07-15: `./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled cppcheck、findings none）。
- 2026-07-15: pass-10 HTTP/2 / gRPC domain model review PASS（初回Medium 1件をterminal quarantineとmultiplex回帰probeで修正後、Blocker / High / Medium / Low / Design Decision: none）。記録は`docs/reviews/issues/2026-07-15-1xx-pass9-fix-domain-model-pass10.md`。
- 2026-07-15: pass-11 fix後に`docker compose build test-server`と`docker compose up -d --force-recreate test-server`を実行し、small-window / large-sibling / peer-read-stall、peer-side CANCEL marker、wire byte-counter reset controlsを含むraw fixture imageのrebuild / restart PASS。
- 2026-07-15: `./tools/test/check-phpt.sh` PASS（29/29 tests、failed 0、skipped 0、warned 0）。PHPT 042でdeadlineなしtargetの500ms以内の`UNKNOWN`、既存siblingの`UNAVAILABLE`、peer受信target CANCEL、quarantine後のsibling DATA不在、fresh connection follow-upを確認した。PHPT 043でentry / byte counter各controlが2 iterationsとも`ok=2 / failed=0`となることを確認した。
- 2026-07-15: `./tools/test/check-c-unit.sh` PASS（protocol_core / response_header_phase / status_core / transport_core、4/4 suites）。
- 2026-07-15: `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions）。
- 2026-07-15: `./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled cppcheck、findings none）。
- 2026-07-15: pass-11 fixのHTTP/2 / gRPC domain-model再確認はBlocker / High / Medium / Low / Design Decisionすべてnone。terminal quarantineのconnection scope、target/sibling lifecycle、peer-side wire oracle、diagnostic iteration resetの責務境界を静的照合した。
- 2026-07-15: pass-13 bookkeepingの静的照合で、`response_header_block_incomplete`のshared producer / production・diagnostic consumer / reset lifetime、3種のEND_HEADERS未完了control、budget-trigger multiplex、authority-keyed follow-upのcaller-selected target RST / post-target-RST DATA境界を設計・fixture・verification資料へ反映した。failure-driven PHPT修正後はpre-target-RST DATA必須条件、client trace / peer marker双方のpost-target-RST DATA不在、target RSTから`rpc.end`までのfinite boundもcurrent docsへ揃えた。
- 2026-07-15: pass-13最終`./tools/test/check-phpt.sh` PASS（29/29 tests、failed 0、skipped 0、warned 0）。初回はmultiplexのend-to-end 500ms assertionが合法なpre-quarantine backpressureを含んで失敗し、target RST以後を測るclient trace oracleへ修正後にfocused PHPT 042と全PHPTがPASSした。
- 2026-07-15: pass-13最終`./tools/test/check-c-unit.sh` PASS（protocol_core / response_header_phase / status_core / transport_core、4/4群）。
- 2026-07-15: pass-13最終`docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions）。
- 2026-07-15: pass-13最終`./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled cppcheck、findings none）。
- 2026-07-15: pass-15 fix後に`docker compose up -d --build --force-recreate test-server`を実行し、regular-before-status normal / invalidとinvalid incomplete-budget controlsを含むraw fixture imageのrebuild / restart PASS。
- 2026-07-15: invalid callbackのEND_HEADERS mutationではfocused PHPT 042 / 043がproduction fresh-follow-up / diagnostic fd-state assertionでFAILし、diagnostic normal callbackのterminal-action consumer削除mutationではPHPT 043がfd-state assertionでFAILした。各mutationを復元後、focused PHPT 042 / 043は2/2 PASS。
- 2026-07-15: pass-15最終`./tools/test/check-phpt.sh` PASS（29/29 tests、failed 0、skipped 0、warned 0）。
- 2026-07-15: pass-15最終`./tools/test/check-c-unit.sh` PASS（protocol_core / response_header_phase / status_core / transport_core、4/4群）。
- 2026-07-15: pass-15最終`docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions、failures 0、errors 0、skipped 0）。
- 2026-07-15: pass-15最終`./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled cppcheck、findings none）。
- 2026-07-15: pass-15 fixのHTTP/2 / gRPC domain model review PASS（Blocker / High / Medium / Low / Design Decision: none）。記録は`docs/reviews/issues/2026-07-15-1xx-pass15-fix-domain-model.md`。
- 2026-07-15: pass-17 fix後に`docker compose up -d --build --force-recreate test-server`を実行し、empty-name exact HPACK controlを含むtest-server imageのrebuild / restart PASS。containerはrunning、RestartCount 0。
- 2026-07-15: pass-17最終`./tools/test/check-phpt.sh` PASS（29/29 tests、failed 0、skipped 0、warned 0）。PHPT 042 / 043でempty-name invalid regular-before-`:status`のproduction / diagnostic finite terminal pathを確認した。
- 2026-07-15: pass-17最終`./tools/test/check-c-unit.sh` PASS（protocol_core / response_header_phase / status_core / transport_core、4/4群）。empty name / pseudo-header / regular nameのpure predicate分類を含む。
- 2026-07-15: pass-17最終`docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions、failures 0、errors 0）。
- 2026-07-15: pass-17最終`./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled cppcheck、findings none）。
- 2026-07-15: pass-17 fixのHTTP/2 / gRPC domain model review PASS（Blocker / High / Medium / Low / Design Decision: none）。記録は`docs/reviews/issues/2026-07-15-1xx-pass17-fix-domain-model.md`。
- 2026-07-15: pass-19 fix後に`docker compose up -d --build --force-recreate test-server`を実行し、strict-invalid pseudo / uppercase regularのexact HPACK controlsを含むtest-server imageのrebuild / restart PASS。containerはrunning。
- 2026-07-15: pass-19初回`./tools/test/check-phpt.sh`は28/29 PASSで、PHPT 042がinvalid informational entry budgetのprimary taxonomy退行（expected `RESOURCE_EXHAUSTED`, got `INTERNAL`）を検出した。invalid-frame producer-owned分岐を`metadata_too_large || NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE`へ修正後、再実行は29/29 PASS（failed 0、skipped 0、warned 0）。strict-invalid pseudo / uppercase regularのunary / server streaming finite `INTERNAL`、exact `PROTOCOL_ERROR`、terminal connection、fresh follow-upとdiagnostic parityを含む。
- 2026-07-15: pass-19最終`./tools/test/check-c-unit.sh` PASS（protocol_core / response_header_phase / status_core / transport_core、4/4群）。declared / unknown field class × `NONE` / `AWAITING_STATUS` / `INFORMATIONAL` / `FINAL_INITIAL` / `TRAILING`のclosed route tableを含む。
- 2026-07-15: pass-19最終`docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions、failures 0、errors 0）。
- 2026-07-15: pass-19最終`./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled cppcheck、findings none）。
- 2026-07-15: pass-19 fixのHTTP/2 / gRPC domain model review PASS（初回Low 1件のCANCEL専用commentをpending control frame全般へ修正後、Blocker / High / Medium / Low / Design Decision: none）。記録は`docs/reviews/issues/2026-07-15-pass19-class-closure-domain-review.md`。
- 2026-07-15: pass-21 fix後に`docker compose up -d --build --force-recreate test-server`を実行し、same-write late incomplete HEADERS controlを含むtest-server imageのrebuild / restart PASS。
- 2026-07-15: pass-21 `./tools/test/check-phpt.sh` PASS（29/29 tests、failed 0、skipped 0、warned 0）。PHPT 042でunary / server streamingの先行call exact OK、closed streamへのRST誤帰属なし、同一clientのfresh connection follow-upを、PHPT 043で1件目OK、2件目未submit、timeoutなし有限terminalを確認した。
- 2026-07-15: pass-21 `./tools/test/check-c-unit.sh` PASS（protocol_core / response_header_phase / status_core / transport_core、4/4群）。unowned incomplete HEADERSのconnection-terminal pure predicate truth tableを含む。
- 2026-07-15: pass-21 `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions、failures 0、errors 0）。
- 2026-07-15: pass-21 `./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled cppcheck、findings none）。
- 2026-07-15: pass-21 fixのHTTP/2 / gRPC domain model review PASS（Blocker / High / Medium / Low / Design Decision: none）。記録は`docs/reviews/issues/2026-07-15-pass21-late-closed-stream-domain-review.md`。
- 2026-07-16: pass-21 finalization再検証として`docker compose up -d --build --force-recreate test-server` PASS、`./tools/test/check-phpt.sh` PASS（29/29 tests、failed 0、skipped 0、warned 0）、`./tools/test/check-c-unit.sh` PASS（4/4群）、PHPUnit PASS（31 tests / 116 assertions）、`./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled findings none）。既存の2026-07-15検証結果と同じ結果となった。
- 2026-07-16: pass-23 finalizationとして`docker compose up -d --build --force-recreate test-server` PASS。`./tools/test/check-phpt.sh` PASS（29/29 tests、failed 0、skipped 0、warned 0）、`./tools/test/check-c-unit.sh` PASS（protocol_core / response_header_phase / status_core / transport_core、4/4群）、PHPUnit PASS（31 tests / 116 assertions）、`./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled findings none）。PHPT 042でlive fragmented informational HEADERS中のunary deadlineとlive fragmented trailing HEADERS中のexplicit cancelについてprimary status、exact `RST_STREAM(CANCEL)`、dead connection destroy、same-client fresh-preface follow-upを、PHPT 043でshared abandonment predicate、session-terminal marker、nonblocking finite teardownを確認した。
- 2026-07-16: pass-23 fixのHTTP/2 / gRPC domain model review PASS。Blocker / High / Medium / Lowはすべてnone。field-class tableを拡張せずcommon teardown seamでconnection lifecycleへhandoffするDesign Decision 1件は、call taxonomy / RST ownershipとconnection scopeを分離する既存モデルの延長としてAccepted。記録は`docs/reviews/issues/2026-07-16-pass23-abandonment-domain-review.md`。

## Decision Log

- 2026-07-10: 記録のみの issue 分割（コードは PR #28 同梱のまま）としたが、第三パスレビューで不完全性が実証されたため、コードごと本 issue の別 PR スコープへ変更。
- 2026-07-15: nghttp2がHEADERS rejectionとDATA-after-1xxで異なるcallbackを通るため、HTTP response messaging violationはDATA framingの `malformed_response_frame` と混ぜず `response_header_protocol_error` とした。HEADERSは `on_invalid_frame_recv_callback()`、DATA-after-1xxはnghttp2が生成するoutbound `RST_STREAM(PROTOCOL_ERROR)` observerで捕捉し、重複RSTはsubmitしない。
- 2026-07-15: final-response後のTrailing HEADERSはbegin callback時点でEND_STREAM flagを読めるため、END_STREAMなしblockのstatus/metadataはその場でquarantineする。nghttp2のblock全体validation後に別の違反が分かった場合はprotocol-error markerが先行statusより優先するため、header field全体のstaging allocationは追加しない。
- 2026-07-15: semantic metadata map用counterをinformational fieldで増やすとownershipが混ざるため、独立したwire header entry/byte counterを導入。configured metadata hard limitと128-entry limitを全decoded response fieldのwork budgetとしても使う。
- 2026-07-15: phase begin / `:status` / end / resetとstatusのEND_STREAM commit predicateをnghttp2 / Zend非依存のpure helperに置き、production / bench diagnosticの構造的parityをC unit transition / truth tableで守る。
- 2026-07-15: terminal semantic failureはheader block完了を待たずfield callbackでtransport actionへ移す。END_HEADERS未完了のinbound HPACK blockはRST_STREAMだけでは同期を回復できないためconnectionをdrainingとし、新規RPCへ再利用しない。
- 2026-07-15: Trailers-Only ownershipは`grpc-status`専用flagではなく、3種類のterminal status fieldが共有するblock-local candidateで決める。missing `grpc-status`の`UNKNOWN` taxonomyとは独立に、同じblockのmetadata roleを一つに保つ。
- 2026-07-15: END_HEADERS未完了のinbound HPACK blockはconnection-globalなdecoder同期を失うため、上記draining判断をterminal quarantineへ更新する。GOAWAY drainingはadmit済みstreamの完走を許すが、このcaseは対象CANCELのpending frameをflushした後にdead化し、全ownerの追加I/Oを止める。cache entryの破棄はactive ownerのcleanupへ委ね、send helper内で即時解放しない。
- 2026-07-15: valid responseで`:status`より前にregular fieldは置けないため、`AWAITING_STATUS`でnormal / invalid regular fieldを観測した時点をresponse-header protocol failureの確定点とする。wire budgetを先に適用してresource taxonomyを優先し、END_HEADERS付きblockはnghttp2の既存block-end rejection、未完了blockはcaller-selected `PROTOCOL_ERROR`を共有terminal actionへ渡す。
- 2026-07-15: response header notificationのclass closureは、applicationへ公開されたnormal / recoverably-invalid fieldをshared budget ownerの後にclosed field-class × phase tableへ写像し、非公開のstrict field rejectionとblock-end rejectionを`REJECTED` defaultへ写像する構造で守る。unknown classはfail closedする。incomplete HPACK lifecycle markはcall taxonomy / RST code / submit ownerから分離し、client-owned RSTはmark + submit、nghttp2-owned RSTはmarkのみとする。production / diagnosticはclassificationを共有し、connection quarantineとone-shot fd nonblockingというscope別consumerだけを分ける。
- 2026-07-15: live callを持たないclosed / foreign streamのHEADERSはfield-class × call phase tableへsynthetic rowを追加しない。field notificationより前のHTTP/2 connection/session lifecycleとして`on_begin_frame`で観測し、`HEADERS && !END_HEADERS && !has_live_call`のshared pure predicateからconnection-terminal actionへ写像する。active streamの合法なfragmented HEADERSはcall-local routeをownerとし、late frameを完了済みcall、`current_read_call`、siblingへ帰属させない。
- 2026-07-16: active streamのfragmented response HEADERSはcall-local phaseをownerとし続けるが、そのcallをblock完了前に放棄する場合はteardown seamがownershipをconnection lifecycleへhandoffする。`block_phase != NONE`をshared pure predicateで判定し、primary statusとcaller-selected RST codeを変えずに既存incomplete-header terminal markを適用する。productionのlive local abandonment（unary / server-streaming deadline、explicit cancel、resource destructor、semantic-error cancel）は共通`cancel_grpc_call_stream()`のRST submit前で適用し、fatal I/O errorは従来どおり先にconnectionをdead化する。normal close / GOAWAY unregisterはbookkeepingのままとし、raw diagnosticは同じpredicateをsticky session terminalへ写像する。

## Close Criteria

- 1xx を挟む応答で metadata ownership / field isolation / status が正しいことを固定する PHPT が unary / server streaming で通る。
- 既存スイート（C unit / PHPT / PHPUnit）に回帰がない。
