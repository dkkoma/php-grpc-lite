# 1xx informational response adversarial consolidated pass 15 2026-07-15

## Scope

- Commits `20c2dc0` / `6a4902f` / `0e22a8a` / `a80556f` / `6168e2e` / `bf1f324` / `712df8a` / `6470c7f`（current HEAD）
- `src/response_header_phase.[ch]`
- `src/grpc_exchange_state.h`
- `src/transport_core.[ch]`
- `src/status_core.c`
- `src/transport.[ch]`
- `src/diagnostic/bench.c`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `poc/test-server/main.go`
- `tests/phpt/022-error-and-http-validation.phpt`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- `tests/unit/test_response_header_phase.c`
- `tests/unit/test_status_core.c`
- `tests/unit/test_transport_core.c`
- 仕様issue、pass-1 / pass-3 adversarial review 6 records、pass-2 / pass-4 domain gate、pass-5〜pass-13 consolidated review / gate records、関連design / verification docs

## Reviewer Role

- consolidated adversary（HTTP/2 / gRPC protocol + C safety / lifetime + test / fixture）

## Review Prompt Summary

- pass 15 convergence checkとして、pass-3修正のshared wire-header budget owner、`NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE`と`RESOURCE_EXHAUSTED`のpriority、diagnostic iteration reset、terminal status gate、pushed-stream attribution、PHPT 042 / 043の識別力をcurrent HEADで再監査した。
- 最新increment `6470c7f`のshared incomplete-header terminal actionを、informational END_STREAM、END_STREAMなしtrailing、normal / invalid header budget overflow、production connection quarantine、raw diagnostic nonblocking finish、multiplex fixtureのpre / post target RST境界まで追跡した。
- issue Decision Logで受容済みのblock staging非採用、wire budget分離、incomplete HPACK blockのconnection-terminal判断、pure phase helperは再議論していない。指示どおりtest suite / Dockerは実行せず、runtime確認が必要なfindingにはexact wire probeを記載した。

## Issues

### REVIEW-20260715-001: `AWAITING_STATUS`のfragmented blockでregular fieldが先行するとprotocol failure確定後もcallが停止する

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `consolidated adversary（HTTP/2 / gRPC protocol + C safety / lifetime）`
- Finding: validなinformational responseの後、次のresponse HEADERSが`:status`より先にregular fieldをdecodeした時点で、pseudo-header ordering上そのblockはvalidなfinal responseになれない。しかしproduction / diagnosticはいずれもphaseを`AWAITING_STATUS`のまま維持してfieldをsemantic stateから無視するだけであり、HEADERSがEND_HEADERSを持たずpeerがCONTINUATIONを送らない場合、missing `:status`のprotocol failureを確定せずdeadline-less callが停止する。`6470c7f`で一般化したincomplete-header terminal actionは、informational END_STREAM / non-terminal trailing / budget overflow / status fieldからしか呼ばれず、この早期確定可能なmalformed sequenceへ接続されていない。
- Evidence: productionの`on_header_callback()`はwire budget計上後、`:status`以外のfieldについて`response_header_phase.block_phase == GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS`なら0を返す（`src/transport.c:2410-2417,2418-2447`）。`on_invalid_header_callback()`も同じphaseを見ずbudget計上だけを行う（`src/transport.c:2520-2539`）。diagnosticも同型である（`src/diagnostic/bench.c:388-433,514-534`）。nghttp2 v1.69.0の[`http_response_on_header()` / `nghttp2_http_on_response_headers()`](https://raw.githubusercontent.com/nghttp2/nghttp2/v1.69.0/lib/nghttp2_http.c)はregular fieldを受理した時点でpseudo-headerを以後禁止する一方、必須`:status`欠落を判定するのはheader block完了時である。同versionの[`inflate_header_block()` / `session_after_header_block_received()`](https://raw.githubusercontent.com/nghttp2/nghttp2/v1.69.0/lib/nghttp2_session.c)もfield callbackをblock完了判定より先に呼ぶため、CONTINUATIONが来なければinvalid-frame observerへ進まない。exact probeは `HEADERS(:status 103, END_HEADERS)` → `HEADERS(x-after: v, END_STREAM, END_HEADERSなし)` → CONTINUATIONなしでTCPをopenのまま保持するsequenceである。unaryは`src/unary_call.c:201-260`、server streamingは`src/server_streaming_call.c:268-340`、default blocking raw batchは`src/diagnostic/bench.c:1697-1719`でstream closeまたはsocket eventを待ち続ける。既存`informational-then-missing-status` fixtureはEND_HEADERS付きなのでnghttp2のblock-end rejectionへ到達し、このfragmented形を識別しない。
- Expected model: response blockの最初のnormal / silently ignored invalid regular fieldを`AWAITING_STATUS`で観測した時点で、「必須`:status`がなく、後続pseudo-headerも合法に置けない」response-header protocol failureを確定する。`response_header_protocol_error`をprimary taxonomyとして`INTERNAL`へ写像し、共有`grpc_protocol_apply_response_header_terminal_action(..., NGHTTP2_PROTOCOL_ERROR)`へ渡す。END_HEADERSなしなら`response_header_block_incomplete`、target RST、production connection terminal quarantine、diagnostic finite finishへ同じownerから遷移する。
- Why it matters: hostileまたは壊れたpeerが、既にvalid responseになれないことをclientへ示した後もCONTINUATIONを省くだけで、timeoutなしPHP workerを保持できる。inbound HPACK decoderもblock途中のままなので、target streamだけを放棄してconnectionを再利用することはできず、pass-9〜pass-13で定義したcall / connection lifecycle invariantにも違反する。
- Recommended fix: normal / invalid regular-header callbackで、budget accounting後かつ`AWAITING_STATUS`のfieldが`:status`でない場合にdedicated protocol classificationとshared terminal actionを実行する。上記raw controlを追加し、unary / server streamingでdeadlineなしの有限`INTERNAL`、target `RST_STREAM(PROTOCOL_ERROR)`、connection terminal化、fresh follow-upを、PHPT 043で`failed=1` / `timed_out=false` / `stream_error_code=1`を固定する。
- Fix summary: production / diagnostic共通のnormal / invalid header callbackでwire budget計上を先に行い、budget内のregular fieldを`GRPC_RESPONSE_HEADER_BLOCK_AWAITING_STATUS`で観測した場合に`grpc_protocol_reject_response_regular_header_before_status()`へ渡すようにした。helperは`response_header_protocol_error`を確定し、END_HEADERS未完了時は既存の`grpc_protocol_apply_response_header_terminal_action(..., NGHTTP2_PROTOCOL_ERROR)`へ接続してtarget `RST_STREAM(PROTOCOL_ERROR)`、production connection terminal quarantine、diagnostic finite finishを共有する。END_HEADERS付きblockはcall-local protocol errorだけを確定し、既存のnghttp2 block-end rejectionへ委ねる。valid / NUL-bearing invalid regular fieldのfragmented controlsを追加し、unary / server streaming / diagnosticのnormal・invalid callback経路を固定した。
- Fix commit: `pending`
- Verification: production / diagnosticのnormal・invalid callbackでbudget-first priorityとshared reject helperへの接続を静的確認した。PHPT 042でvalid / invalid regular-before-statusのunary・server streamingがdeadlineなしで有限の`INTERNAL`、既定details、target `RST_STREAM(PROTOCOL_ERROR)`、fresh connection follow-upになることを確認した。PHPT 043で`failed=1`、`timed_out=false`、`stream_error_code=1`、invalid callback count 1、default-blocking batchのterminal action後に実fdが`O_NONBLOCK`であることを確認した。required suite結果は下記Verification節に記録した。
- Notes: completeなmissing-`:status` blockを捕捉するpass-1 protocol findingや、pass-13で修正した3 triggerの再掲ではない。block completion前でもpseudo-header orderingからfailureを確定できる別edgeで、旧findingのfixが不十分であることを示す。

### REVIEW-20260715-002: incomplete wire-budget oracleがinvalid-header producerとdiagnostic nonblocking consumerを識別しない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `consolidated adversary（test / fixture）`
- Finding: `6470c7f`はnormal / invalid header callbackのbudget overflowからframe flagsをshared incomplete-header actionへ渡し、raw diagnosticでは`response_header_block_incomplete`を読んでfdをnonblocking化する。しかし新規fixture / PHPTはvalid `x-info`によるincomplete entry overflowを低backpressureのone-shot batchへ送るだけである。そのため (a) invalid-header callbackだけがEND_HEADERS未完了をterminal connection actionへ渡さない退行と、(b) diagnostic budget callbackだけが`bench_finish_response_header_terminal_action()`を呼ばない退行の双方を既存gateが識別しない。
- Evidence: production / diagnosticのinvalid callbackは現在、frame flags付きでshared budget ownerを呼ぶ（`src/transport.c:2520-2539`、`src/diagnostic/bench.c:514-534`）。一方、incomplete budget control `writeRawIncompleteInformationalEntryBudget()`はvalid `x-info`だけを使う（`poc/test-server/main.go:935-936,1131-1136`）ためnormal callbackしか通らず、invalid entry / byte controlsはEND_HEADERS付きである（`poc/test-server/main.go:946-959`）。従ってinvalid callbackだけ`frame->hd.flags | NGHTTP2_FLAG_END_HEADERS`をshared ownerへ渡すmutationでもPHPT 042 / 043は通り得る。またdiagnostic normal callbackの`rv = bench_finish_response_header_terminal_action(call, rv)`だけを削除するmutation（`src/diagnostic/bench.c:389-390`）も、budget ownerが`712df8a`時点から既に`metadata_too_large`、`RST_STREAM(CANCEL)`、TEMPORALを確定しており、fixtureが小さいHEADERS送出後すぐactive read loopへ戻るため、PHPT 043の`ok=0` / `failed=1` / `timed_out=false` / code 8 / 500ms未満を満たし得る（`tests/phpt/043-informational-1xx-bench-parity.phpt:71-82`）。`docs/verification/verification-matrix.md`はinvalid callbackとincomplete-block diagnostic actionをcoveredとしているが、このproducer / consumerを個別には固定していない。
- Expected model: shared incomplete-header actionのregression gateは、normal / invalid callbackというproducer境界と、production connection quarantine / diagnostic nonblocking finishというconsumer境界を区別する。END_HEADERS未完了のinvalid-field overflowはconnectionを再利用せず、diagnosticはpeerのread progressやsocket send backpressureに依存せず終了する。
- Why it matters: 前者が退行すると、NUL-bearing等のsilently ignored invalid fieldでbudget超過したtargetは`RESOURCE_EXHAUSTED`とCANCELだけを返しても、HPACK decoderがCONTINUATION待ちのconnectionを再利用可能として残す。後者が退行すると、分類とsmall RSTだけを見る現PHPTは成功する一方、送信bufferが詰まったone-shot diagnosticはblocking sendで停止できる。current implementationのruntime defectではないが、newest incrementの2つの必須edgeをverification済みとするoracleがpre-fix相当mutationを落とせない。
- Recommended fix: raw fixtureへ `HEADERS(:status 103 + NUL-bearing invalid regular fieldsでentry超過, END_HEADERSなし)` → CONTINUATIONなしのcontrolを追加し、productionの`RESOURCE_EXHAUSTED`、peer-received CANCEL、terminal connection、fresh follow-upとdiagnostic resultを固定する。diagnostic側は、peerがreadを止めたsend-backpressure controlでnonblocking finishの有限性を直接検証するか、test-only observationとして`response_header_block_incomplete`による`O_NONBLOCK` transitionを結果へ露出してPHPT 043でassertする。上記2 mutationをfocused mutation probeとして実行する。
- Fix summary: `:status: 103`とNUL-bearing invalid regular field 129個を同じEND_HEADERS未完了blockで送る`informational-incomplete-invalid-entry-budget`を追加し、invalid-header callback producerを独立して観測できるようにした。production PHPTは`RESOURCE_EXHAUSTED`、target `RST_STREAM(CANCEL)`、terminal connection、fresh follow-upを固定し、diagnostic PHPTはinvalid callback cutoff 128回を固定した。さらにdiagnostic batch resultへpost-batch `F_GETFL`による実fdの`O_NONBLOCK`観測を追加し、shared incomplete-header terminal actionのnonblocking consumerをpeer read progressから独立してassertするようにした。
- Fix commit: `pending`
- Verification: invalid callbackだけbudget ownerへEND_HEADERSありとして渡すmutationではfocused PHPT 042がfresh follow-up assertion、PHPT 043が`incomplete_header_fd_nonblocking` assertionで失敗し、invalid incomplete producer oracleの識別力を確認した。diagnostic normal callbackから`bench_finish_response_header_terminal_action()`を除くmutationではPHPT 043の同fd-state assertionが失敗し、nonblocking consumer oracleの識別力を確認した。各mutationを復元後、focused PHPT 042 / 043とrequired suiteがPASSした。
- Notes: informational / trailing incomplete controlsはpre-fixに即時RST自体がなかったため現PHPT 043が識別する。Lowは、pre-fixにもCANCEL + TEMPORALがあったbudget pathと、未実行のinvalid-header × incomplete-block交差に限定する。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`（1件Fixed）
- Low: `none`（1件Fixed）
- Design Decision: `none`

## Progress

- 2026-07-15: pass-15 Mediumに対し、`AWAITING_STATUS`でnormal / invalid regular fieldが`:status`より先行した時点をshared protocol classificationへ追加した。wire budgetを先に適用し、END_HEADERS未完了時だけ既存incomplete-header terminal actionへ`PROTOCOL_ERROR`を渡すことで、complete blockの既存nghttp2 rejectionとincomplete blockのconnection-terminal lifecycleを分離した。
- 2026-07-15: pass-15 Lowに対し、invalid-header callback × END_HEADERS未完了budget controlと、default-blocking diagnostic fdの実`O_NONBLOCK` state oracleを追加した。producer / consumerを個別にmutationし、PHPT 042 / 043が各退行を識別することを確認した。
- 2026-07-15: SPEC、exchange-state / protocol-classification design、code-reading guide、fixture catalog、verification matrix、compatibility checklistをregular-before-status、invalid incomplete-budget、complete / incomplete connection lifecycle、diagnostic fd-state観測へ更新した。

## Verification

- 2026-07-15: `docker compose up -d --build --force-recreate test-server` PASS。3つの新raw controlを含むtest-server imageをrebuild / restartした。
- 2026-07-15: invalid callbackのbudget ownerへ`END_HEADERS`を強制するmutationでfocused PHPT 042 / 043が期待どおりFAILし、production fresh-follow-upとdiagnostic fd-stateのproducer oracleを確認した。
- 2026-07-15: diagnostic normal callbackから`bench_finish_response_header_terminal_action()`を除くmutationでfocused PHPT 043が期待どおりFAILし、nonblocking consumer oracleを確認した。両mutationの復元後、focused PHPT 042 / 043は2/2 PASSした。
- 2026-07-15: `./tools/test/check-phpt.sh` PASS（29/29 tests、failed 0、skipped 0、warned 0）。
- 2026-07-15: `./tools/test/check-c-unit.sh` PASS（protocol_core / response_header_phase / status_core / transport_core、4/4群）。
- 2026-07-15: `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions、failures 0、errors 0、skipped 0）。
- 2026-07-15: `./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled cppcheck、findings none）。
- 2026-07-15: HTTP/2 / gRPC domain model review PASS（Blocker / High / Medium / Low / Design Decision: none）。記録は`docs/reviews/issues/2026-07-15-1xx-pass15-fix-domain-model.md`。
