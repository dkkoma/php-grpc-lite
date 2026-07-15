# 1xx informational response adversarial consolidated pass 13 2026-07-15

## Scope

- Commits `20c2dc0` / `6a4902f` / `0e22a8a` / `a80556f` / `6168e2e` / `bf1f324` / `712df8a`（current HEAD）
- `src/response_header_phase.[ch]`
- `src/grpc_exchange_state.h`
- `src/transport_core.[ch]`
- `src/status_core.c`
- `src/transport.[ch]`
- `src/diagnostic/bench.c`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `poc/test-server/main.go`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- response phase / transport / fixture関連docs、pass-1〜pass-11 review / gate records、仕様issue

## Reviewer Role

- consolidated adversary（HTTP/2 / gRPC protocol + C safety / lifetime + test / fixture）

## Review Prompt Summary

- pass 13 convergence checkとして、pass-3以降のshared wire-header budget、`NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE`と`RESOURCE_EXHAUSTED`のpriority、terminal status gate、pushed-stream attribution、call / connection lifetime、production / diagnostic parityをcurrent HEADで再監査した。
- 最新increment `712df8a`の固定50ms terminal-quarantine flush、sibling DATA defer、peer-received CANCEL marker、wire byte-counter iteration resetを重点的に追跡し、既存probeがpre-fix behaviorを識別するかも確認した。
- issue Decision Logで受容済みのblock staging非採用、独立wire budget、pure phase helper、invalid-frame / outbound protocol-RST observerは再議論していない。指示どおりtest suite / Dockerは実行せず、runtime確認が必要な点はexact wire probeとして記載した。

## Issues

### REVIEW-20260715-001: END_HEADERS未完了blockのterminal quarantineがstatus-field経路に限定されている

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `consolidated adversary（protocol + C safety / lifetime）`
- Finding: `close_after_pending_flush`、固定50ms grace、sibling DATA deferへ接続されるのはEND_STREAMなし`FINAL_INITIAL`のstatus field observerだけである。同じくfield / begin callback時点でfailureが確定する (a) `103 + END_STREAM`、(b) END_STREAMなしTrailing HEADERS、(c) wire-header budget超過は、HEADERSが`END_HEADERS`を持たずpeerがCONTINUATIONを送らない場合でもterminal quarantineへ入らない。前二者はdeadlineなしcallが既確定の`INTERNAL`を返せず停止し、budget caseは通常のsession-wide flushでpass-11と同じsibling DATA / socket-backpressure stallを再現できるうえ、flushできてもinbound HPACK stateを完了できないconnectionを再利用可能なまま残す。
- Evidence: status-field専用owner `src/transport.c:2327-2363` だけが`frame_flags`の`END_HEADERS`を検査し、`response_header_block_incomplete`とconnectionのclose-after-pending-flushを設定してCANCELをqueueする。一方、`on_begin_headers_callback()`のEND_STREAMなしTrailing rejection (`src/transport.c:2285-2295`) と`:status` callbackのinformational END_STREAM rejection (`src/transport.c:2395-2399`) は`response_header_protocol_error` / body discardだけで0を返す。blockが完了しないため`on_invalid_frame_recv_callback()` / `on_frame_recv_callback()`へ到達せず、`src/unary_call.c:201-260`と`src/server_streaming_call.c:268-340`は再びrecvを待つ。wire budget owner `src/transport.c:2299-2324` はframe flagsを受け取らず、超過時に`metadata_too_large`、`RST_STREAM(CANCEL)`、TEMPORALだけを確定するため、`src/transport.c:2100-2107,2144-2149`のquarantine grace / DATA deferを通らない。diagnostic側も`src/diagnostic/bench.c:336-344,393-401`の前二者ではsocketをnonblockingへ移さず、default blocking batchが同じsilent peerを待てる。exact probesは (a) `HEADERS(:status 103, END_STREAM=1, END_HEADERS=0)`、(b) valid final response後の`HEADERS(END_STREAM=0, END_HEADERS=0)`、(c) flow-control deferredな16MiB siblingとWINDOW_UPDATEの後にbudget超過fieldを含む`HEADERS(END_HEADERS=0)`を送り、いずれもCONTINUATIONを送らずsocketをopenにするsequenceである。既存fixtureのincomplete control集合は3 status fieldだけ (`poc/test-server/main.go:1092-1101`) で、103 / trailing / budget controlsはEND_HEADERS付きである。
- Expected model: END_HEADERS未完了のinbound HPACK blockでcall-local terminal failureが確定した場合、failure taxonomyやRST error codeにかかわらずconnection-terminal lifecycleを一つのownerへ集約する。対象streamのRSTを固定grace内でbest-effort flushし、quarantine後のapplication DATAをdriveせず、connectionをdead化する。対象callは前二者で`INTERNAL`、budget超過で`RESOURCE_EXHAUSTED`を維持し、既存siblingは`UNAVAILABLE`、follow-upはfresh connectionへ進む。
- Why it matters: malformedまたはheader-heavyなpeerが、pass-9 / pass-11で閉じたはずのdeadlineなしPHP worker stallをstatus field以外の既知failure triggerで再現できる。budget caseではtargetがlocal RSTで終了しても、HPACK decoderがCONTINUATION待ちのconnectionをcacheへ残すため、sibling / follow-upのlivenessとconnection-wide protocol stateも壊れる。
- Recommended fix: classificationとは独立したshared incomplete-header terminal-action helperを追加し、status-field observer、Trailing begin rejection、informational END_STREAM rejection、normal / invalid header budget overflowから`END_HEADERS=0`時に呼ぶ。productionは既存`response_header_block_incomplete` / close-after-pending-flush / 50ms flush / DATA deferへ接続し、raw diagnosticは同じclassificationを保ったままone-shot socketをnonblockingにして有限終了させる。上記3 fragmented controlsを追加し、unary / server streaming / diagnosticのfinite result、peer受信RST、siblingの追加DATAなし`UNAVAILABLE`、fresh follow-upを固定する。
- Fix summary: status-field専用だったEND_HEADERS未完了判定を、call-local terminal classification後に使う共有`grpc_protocol_apply_response_header_terminal_action()`へ集約した。informational END_STREAM、END_STREAMなしtrailing、normal / invalid header callbackのwire budget超過、既存3 status fieldの各経路が、END_HEADERSなしなら`response_header_block_incomplete`とconnection terminal quarantineを同じownerから確定する。primary taxonomyとstream-local RST codeは各classificationのまま維持し、productionは固定graceのcontrol flush後にconnectionをdead化、raw diagnosticは`bench_finish_response_header_terminal_action()`で同じflagを読んでone-shot fdをnonblocking化する。fixture / PHPTへ`incomplete-informational-end-stream`、`incomplete-trailer-without-end-stream`、`informational-incomplete-entry-budget`とbudget-trigger multiplexを追加した。
- Fix commit: `pending`
- Verification: `production / diagnostic callbackが共有incomplete-block actionとcall-local flagを通ること、normal / invalid budget callbackの双方がframe flagsを渡すこと、production callのzero-initialization / diagnostic iteration resetが揃うことを静的照合した。PHPT 042 / 043は3種のEND_HEADERS未完了sequenceについて有限終了、primary taxonomy、peer受信RST、fresh follow-upを固定し、budget-trigger multiplexは既存siblingのUNAVAILABLE、client trace / peer marker双方のpost-target-RST DATA不在、target RSTからrpc.endまで500ms未満を確認する。最終suiteはPHPT 29/29、C unit 4/4群、PHPUnit 31 tests / 116 assertions、C static analysis findings noneでPASSした。`
- Notes: pass-9 `REVIEW-20260715-001`とpass-11 `REVIEW-20260715-001`のstatus-field修正が、同じincomplete HPACK lifecycleを持つ別triggerには適用されていないことを示す新規findingである。Decision Logのconnection-terminal判断自体は変更対象にしていない。

### REVIEW-20260715-002: multiplex fixtureがclient-side quarantine開始前のsibling DATAも違反扱いする

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `consolidated adversary（test / fixture）`
- Finding: peer-side sibling-DATA oracleはmalformed target responseを送る前にprobeを作成し、その後に受信したsibling DATAをすべて`after trigger`として記録する。しかしclientがterminal quarantineへ入る境界は後続のEND_HEADERSなしHEADERSをdecodeした時点であり、先行WINDOW_UPDATEだけを先に受信したclientが通常send pathで送る合法なpre-quarantine DATAまでfailureにする。現在のPASSはWINDOW_UPDATEとmalformed HEADERSが同じclient `recv()` / `nghttp2_session_mem_recv()`へ入ることに依存する。
- Evidence: `poc/test-server/main.go:764-769`は`rawResponseLeavesHeaderBlockIncomplete()`を見てresponse送出前にprobeをbeginする。multiplex responseはconnection / sibling WINDOW_UPDATEを別frameとして先にwriteし (`poc/test-server/main.go:997-1004`)、その後にEND_HEADERSなしHEADERSをwriteする。fixture read loopはprobe作成後のsibling DATAをCANCEL観測前でも無条件に`observeData()`へ渡し (`poc/test-server/main.go:824-840`)、`passed()`はその1件だけでfalseになる (`poc/test-server/main.go:664-670`)。exact discriminatorは2個目のWINDOW_UPDATEとmalformed HEADERSの間へ短いdelayを置くsequenceであり、TCP segmentationでも同じ分割が起こり得る。clientはまだ`close_after_pending_flush == false`なので`src/unary_call.c:253-259`の通常sendでsibling DATAを送ってよいが、750ms後にfixtureがDATA、target CANCELの順で読むとPHPT 042のfollow-up (`tests/phpt/042-informational-1xx-adversarial.phpt:273-278`) がfixture errorになる。
- Expected model: oracleが禁止するのはclientがincomplete HEADERSを観測してterminal quarantineへ遷移した後に新たにdriveしたsibling application DATAだけであり、server-side probe作成後かつclient-side quarantine前のDATAではない。peer-received target CANCELのmarkerと、client-side transition後のDATA absenceは別の時点を観測する。
- Why it matters: productionのcorrectな通常sendとquarantine deferを維持していても、合法なframe分割だけでPHPT 042が失敗する。逆に現在のfixture resultから「quarantine開始後のDATAなし」というdocs / issueのclaimを直接導けず、最新fixの中心oracleがsocket read boundaryに依存する。
- Recommended fix: client traceへconnection / target streamを持つquarantine-start eventを追加し、その後のsibling DATA frame生成がないことをassertする一方、peer-side storeはtarget CANCELの実受信だけをgateする。traceを増やさない場合は少なくともDATA markerをtarget CANCEL観測後にarmし、pre-fixのno-defer mutationがCANCEL後DATAとして確実に識別されるwire setupへ変更する。WINDOW_UPDATEとmalformed HEADERSが同一recvに入ることへ依存しないdiscriminatorを用意する。
- Fix summary: peer-side markerのsibling DATA違反境界をprobe作成時点ではなくtarget `RST_STREAM(CANCEL)`受信時点へ移した。32MiB WINDOW_UPDATEとmalformed HEADERSを50ms分割し、target RST前の合法なsibling DATAをpositive setup conditionとして必須にする一方、peerがtarget CANCELを観測した後のsibling DATAだけを違反として記録する。PHPT 042のclient traceもtarget RST後のsibling DATA `frame_out`を拒否し、target RSTからtarget `rpc.end`までを500ms未満に固定する。end-to-end wall timeには合法なpre-quarantine backpressureが含まれるためterminal graceのoracleには使わない。budget overflowをtriggerにする`multiplex-incomplete-entry-budget`とauthority-keyed cross-connection follow-up `require-prior-incomplete-status-cancel`が、pre-RST setup、target CANCEL受信、post-CANCEL DATA不在を分離してgateする。
- Fix commit: `pending`
- Verification: `fixture / PHPTの静的照合で、32MiB WINDOW_UPDATE後の50ms分割、pre-target-RST sibling DATA必須条件、target CANCELを境界にしたpeer DATA marker、client traceのpost-target-RST DATA不在とRST-to-rpc.end bound、authority-keyed markerの最大3秒wait / one-shot consume、fresh connection follow-upを確認した。初回PHPTはend-to-end 500ms assertionが合法なpre-quarantine backpressureを含んで失敗したため、terminal graceの計測をclient traceのtarget RST以後へ修正した。修正後のfocused PHPT 042と全PHPT 29/29を含め、指定4 suiteはすべてPASSした。`
- Notes: pass-11 `REVIEW-20260715-002`のpeer-received CANCEL化と同`REVIEW-20260715-001`のsibling DATA deferを結合した`712df8a` fixtureに新しく生じたoracle boundary defectであり、production DATA defer自体へのfindingではない。

### REVIEW-20260715-003: 新しいincomplete-block stateとfollow-up controlがownership / fixture mapに反映されていない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `consolidated adversary（domain / docs consistency）`
- Finding: `712df8a`は`grpc_call.response_header_block_incomplete`とauthority-keyed cross-connection control `require-prior-incomplete-status-cancel`を追加したが、field ownership mapとraw fixture catalogを更新していない。current docsはterminal quarantineとfresh follow-upの結果だけを説明し、call-local flagをproduction共有helperがsetしてbench wrapperがsocket-mode transitionに使うlifetime、およびfollow-upが最大3秒待ってpeer CANCEL / sibling DATA markerをconsumeするfixture責務を示さない。
- Evidence: fieldは`src/grpc_exchange_state.h:75`にあり、shared `grpc_protocol_observe_response_status_field()`がsetし (`src/transport.c:2356-2361`)、bench wrapperがreadしてfdをnonblockingへ変更し (`src/diagnostic/bench.c:347-361`)、iterationごとにresetする (`src/diagnostic/bench.c:1530-1534`)。しかしfield ownership mapを明示的な目的とする`docs/design/grpc-call-exchange-state.md:9-18`のresponse header-block rowは`response_header_phase` / `response_header_block_end_stream` / `response_header_block_protocol_valid`だけを列挙する。またcontrolは`poc/test-server/main.go:754-763`とPHPT 042の3 follow-up箇所で使うが、全raw valuesを列挙する`docs/verification/test-fixtures.md:85-113`にrowがない。対照的に既存marker control `require-prior-resource-probe` / `require-prior-status-probe`は同catalogに記載されている。
- Expected model: `grpc_call` field ownership mapは新しいcall-local stateのproducer、consumer、production / diagnostic boundary、reset lifetimeを列挙する。fixture catalogはPHPTが直接指定するraw controlについて、connectionを跨ぐauthority identity、peer-received target RST、sibling DATA condition、wait / consume semanticsをcurrent behaviorとして記す。
- Why it matters: runtime bugではないが、productionではclassification補助、diagnosticではsocket orchestrationに使う同一fieldの責務差とreset要件が設計資料から消えている。fixture側もfresh follow-upが単なるOK responseではなくprocess-global markerをconsumeすることが分からず、並列化やauthority変更時にoracleを壊しやすい。
- Recommended fix: `docs/design/grpc-call-exchange-state.md`のresponse header-block responsibilityへ`response_header_block_incomplete`を追加し、shared producer / bench consumer / per-call resetを記載する。`docs/verification/test-fixtures.md`へ`require-prior-incomplete-status-cancel` rowを追加し、authority-keyed cross-connection marker、target CANCEL、sibling DATA gate、3秒wait / consumeを明記する。REVIEW-20260715-002の境界修正後はcatalogも最終oracleへ合わせる。
- Fix summary: `docs/design/grpc-call-exchange-state.md`のresponse header-block responsibilityへ`response_header_block_incomplete`を追加し、shared producer、productionのconnection quarantine consumer、diagnosticのnonblocking socket consumer、call / iteration reset lifetimeを明記した。`docs/verification/test-fixtures.md`へ3種のincomplete-block control、budget-trigger multiplex、`require-prior-incomplete-status-cancel`を追加し、authority-keyed cross-connection identity、peer-received caller-selected target RST、post-target-RST sibling DATA boundary、最大3秒wait / consumeを記録した。`docs/verification/verification-matrix.md`もincomplete informational / trailing / budget coverageへ更新した。
- Fix commit: `pending`
- Verification: `current struct / callback / call setup / diagnostic iteration resetとfield ownership map、fixture switch / PHPT 042 / 043とcontrol catalog / verification matrixを静的に再照合した。fixture catalog / verification matrixにはpre-target-RST DATAのpositive setup、client trace / peer markerのpost-target-RST DATA不在、target RSTからrpc.endまでのfinite boundを反映した。`
- Notes: previous field-map findingを再掲するものではなく、`712df8a`で新設されたfield / controlに限定する。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`（1件Fixed）
- Low: `none`（2件Fixed）
- Design Decision: `none`

## Progress

- 2026-07-15: pass-13のMedium 1件について、status-field専用だったEND_HEADERS未完了blockのterminal actionを共有helperへ集約し、informational END_STREAM、END_STREAMなしtrailing、normal / invalid wire-header budget超過をproduction / diagnosticの同じclassificationへ接続した。
- 2026-07-15: pass-13のLow 2件について、multiplex fixtureをpre-target-RST DATA必須・post-target-RST DATA禁止の境界へ修正し、client traceのRST-to-completion boundを追加した。field ownership、fixture catalog、verification matrixも最終oracleへ更新した。

## Verification

- 2026-07-15: 変更済み`poc/test-server/main.go`を含むtest-server imageをrebuildし、containerをrecreate / restartした。
- 2026-07-15: `./tools/test/check-phpt.sh` PASS（29/29 tests、failed 0、skipped 0、warned 0）。
- 2026-07-15: `./tools/test/check-c-unit.sh` PASS（protocol_core / response_header_phase / status_core / transport_core、4/4群）。
- 2026-07-15: `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions）。
- 2026-07-15: `./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled cppcheck、findings none）。
