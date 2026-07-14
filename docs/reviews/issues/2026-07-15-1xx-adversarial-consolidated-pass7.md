# 1xx informational response adversarial consolidated pass 7 2026-07-15

## Scope

- Commits `20c2dc0` / `6a4902f` / `0e22a8a` / `a80556f`（current HEAD）
- `src/response_header_phase.[ch]`
- `src/grpc_exchange_state.h`
- `src/status_core.c`
- `src/transport_core.[ch]`
- `src/transport.[ch]`
- `src/diagnostic/bench.c`
- `src/diagnostic/bench_call.h`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `poc/test-server/main.go`
- `tests/phpt/022-error-and-http-validation.phpt`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- `tests/unit/test_response_header_phase.c`
- `tests/unit/test_status_core.c`
- `tests/unit/test_transport_core.c`
- pass-1 / pass-3 adversarial review 6 records、pass-2 / pass-4 domain-model gate、pass-5 consolidated review、pass-6 domain-model gate
- `docs/issues/open/2026-07-10-informational-1xx-response-handling.md` と関連design / verification docs

## Reviewer Role

- consolidated adversary（HTTP/2 / gRPC protocol + C safety / lifetime + test / fixture）

## Review Prompt Summary

- pass 7 convergence checkとして、pass-3 / pass-5修正のshared wire-header budget、`NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE`と`RESOURCE_EXHAUSTED`のpriority、bench iteration reset、terminal status-field gate、pushed-stream attribution、late-frame lifetime、PHPT 042 / 043の識別力をcurrent HEADで再監査した。
- `a80556f`を重点対象に、silent peerへ送るmain-stream `RST_STREAM(CANCEL)`、その送信失敗時のstatus/details、pre-final budget details、invalid-header callback cutoff、raw fixtureのwire sequenceとfinite guardを静的に追跡した。指示どおりtest suite / Dockerは実行していない。
- issue Decision Logで受容済みのblock staging非採用、独立wire budget、pure phase helper、invalid-frame / outbound protocol-RST observerの選択は再議論していない。

## Issues

### REVIEW-20260715-001: terminal-gate CANCELのflush失敗でstatus codeとdetailsのprimary ownerが分裂する

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `consolidated adversary（protocol + C safety / lifetime）`
- Finding: END_STREAMなし`FINAL_INITIAL`のstatus fieldを検出した新helperは`invalid_grpc_status`を確定して`RST_STREAM(CANCEL)`をqueueする。このRSTのflushがnon-timeout I/O errorで失敗すると、status codeは`invalid_grpc_status`をprimary failureとして`UNKNOWN`に保つ一方、details builderはsecondaryな`last_io_error_detail` / connection detailを先に返す。同じresult内でcodeとdetailsが別のfailure ownerを説明する。
- Evidence: `grpc_protocol_enforce_terminal_initial_status_fields()` (`src/transport.c`) は`initial_grpc_status_seen && !END_STREAM`で`invalid_grpc_status = true` / body discardを確定してCANCELをsubmitする。受信callback後、unaryの`grpc_lite_unary_call_perform_core_on_connection()`とserver streamingの`server_streaming_call_next_resource_core()`はpending frameをflushし、`send_pending_h2_frames_with_deadline()`はsend失敗時にconnectionをdeadにして`call->last_io_error_detail`へI/O detailをsnapshotする。`grpc_lite_status_code_from_call()` (`src/status_core.c`) は`invalid_grpc_status`を`connection_broken`より先に評価して`UNKNOWN`を選ぶが、`grpc_lite_status_details_from_call()` (`src/transport.c`) は`last_io_error_detail` / `connection->last_error_detail`を`invalid grpc-status trailer`より先に返す。exact probeは `103` → END_STREAMなし`FINAL_INITIAL(:status 200, content-type, grpc-status: 0)`（他2 status fieldでも同じ）を送った直後、clientがqueueしたCANCELのflushをnon-timeout `EPIPE` / `ECONNRESET`へするwire/fault sequenceである。current codeでは`UNKNOWN` + `send failed: ...`等になり得る。
- Expected model: public status codeとdetailsは同じprimary call failureを説明する。status taxonomyが`invalid_grpc_status`をconnection teardown failureより優先するなら、CANCEL flushのI/O failureはconnection diagnosticとして保持しつつ、public detailsも`invalid grpc-status trailer`を維持する。
- Why it matters: applicationのcode-based判断はmalformed status lifecycleを示す`UNKNOWN`を見る一方、log / telemetry / operatorはdetailsからtransport send failureをprimary causeと誤認する。通常のCANCEL成功時は正しいため発生条件は限定されるが、`a80556f`が追加したstream-local終了actionのfailure edgeでtaxonomyが自己矛盾する。
- Recommended fix: `grpc_lite_status_details_from_call()`で、少なくとも`code == GRPC_STATUS_UNKNOWN && call->invalid_grpc_status`のdetailsをsecondary I/O fallbackより前に選ぶ。CANCEL submit成功後のflushだけをnon-timeout send failureへするdeterministic fault seamを用意し、unary / server streamingで`UNKNOWN` + `invalid grpc-status trailer`とconnection evictionを固定する。
- Fix summary: `grpc_lite_status_details_from_call()`で、status codeが`UNKNOWN`かつ`invalid_grpc_status`がprimary classificationの場合のdetailsを、wire `grpc-message`、call-local / connection I/O detail、HTTP fallbackより前に選ぶようにした。secondaryなCANCEL flush失敗は`last_io_*`とdead connection stateへ保持したまま、public code/detailsを`UNKNOWN` + `invalid grpc-status trailer`へ揃えた。test-only `terminal-status-rst-flush-fatal` seamは、nghttp2がqueue済みCANCELをcoalesced bufferへ処理した後、socket flush前にnon-timeout `EPIPE`を発生させる。PHPT 044で3 status fieldのunary / server streaming、exact public code/details、dead connection eviction後のfresh follow-upを固定した。
- Fix commit: `pending`
- Verification: `PHPT 044でgrpc-status / grpc-message / grpc-status-details-binの各silent sequenceをunary / server streamingで実行し、CANCEL flushがEPIPEで失敗してもUNKNOWN + invalid grpc-status trailerとなることを確認した。6個のisolated connection identityそれぞれでfault call後のfollow-upがfresh connection上でOKとなり、traceのconnection preface 12件でdead connection evictionを固定した。./tools/test/check-phpt.sh 29/29 PASS、pass-8 domain model gateは全category none。`
- Notes: pass-5 `REVIEW-20260715-001`のsilent-peer terminal action修正が通常経路ではfailure modeを閉じたことは確認した。本指摘はその新しいCANCEL flushが失敗するedgeに限定し、既存のdetails priority一般を再議論しない。

### REVIEW-20260715-002: PHPT 043の2秒guardがblocking recv経路では機能せずsilent regressionでrunner timeoutまでhangする

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `consolidated adversary（test / fixture）`
- Finding: PHPT 043はsilent status-field / wire-budget controlへ`timeout_us=2_000_000`を渡すが、batchをdefaultの`poll_loop=false`で実行する。この経路は接続後のblocking socketで`recv()`し、call deadlineを検査しないため、terminal CANCELやbudget callback wiringが退行してpeerがsilentになると2秒で`timed_out=true`を返さずtest processがblockする。`timed_out=false` assertionはcurrent fixの成功結果を確認するが、記録がいうfinite 2秒guardにはなっていない。
- Evidence: `tests/phpt/043-informational-1xx-bench-parity.phpt`の`$runBatch()`はnamed `timeout_us`だけを渡し、silent status 3 controls、entry / default-byte / invalid-entry budget、foreign pushed-stream controlを同じhelperで実行する。`grpc_lite_bench_unary_batch()` (`src/diagnostic/bench.c`) のdefaultは`poll_loop = false`であり、`connect_tcp()` (`src/transport.c`) はconnect後にfdをblocking modeへ戻して`SO_RCVTIMEO`を設定しない。deadline検査は`drive_stream_poll()`にだけ存在し、default branchは`while (!call.stream_closed) { recv(...) }`でblocking receiveを続ける。実際、pass-5 invalid-header callback no-op mutationは内側2秒ではなく外側10秒guardのexit 124で検出されたと記録されており、このcontrol flowと一致する。
- Expected model: silent-peer regression oracleはtest runner全体のtimeoutに依存せず、指定した短いdeadline内に結果へ収束する。fixありは`failed=1`, `timed_out=false`, exact CANCEL、fixなしは約2秒で`timed_out=true`となってassertion failure、という区別を同じentrypointで持つ。
- Why it matters: 最も重要なliveness regressionである「failure確定後もpeerを待つ」が戻った場合、focused PHPTが決定的かつ短時間に失敗せず、suiteをrunner timeoutまで占有する。current implementationの正常結果は検証できているが、pass-5修正のfinite guardというoracle claimが成立していない。
- Recommended fix: guard対象の`grpc_lite_bench_unary_batch()`呼び出しへ`poll_loop: true`を渡し、deadline-awareな`drive_stream_poll()`を使う。またはblocking branch自体にpoll / `SO_RCVTIMEO`等のdeadline enforcementを追加し、期限到達時の`EAGAIN` / `EWOULDBLOCK`も`timed_out = true`へ写像する。terminal CANCELを外すmutationとdiagnostic invalid-header callbackをno-opにするmutationの双方で、外側timeoutではなく約2秒の`timed_out=true` returnによりPHPTが失敗することを確認する。
- Fix summary: `PHPT 043のローカル$runBatch()で、timeout_usが正値のケースだけpoll_loop=trueを指定するようにした。silent status-field、wire-header budget、foreign pushed-streamの既存2秒guardはdeadline-awareなdrive_stream_poll()を通り、timeoutなしのpositive / nonterminal controlsは従来どおりdefault blocking経路を維持する。blocking recv分岐自体のdeadline semanticsは変更していない。`
- Fix commit: `pending`
- Verification: `PHPT 043で期限付きsilent status / resource controlsがpoll_loop=trueとなり、current実装はdeadline前にfailed=1 / timed_out=false / exact RST_STREAM(CANCEL)へ収束することを確認した。意図的に応答しないTCP peerに対する同entrypoint probeは約2.007秒でfailed=1 / timed_out=trueを返し、外側runner timeoutに依存しないdeadline returnを確認した。./tools/test/check-phpt.sh 29/29 PASS。`
- Notes: PHPT 042のproduction call timeoutは別のdeadline-aware経路を使うため、本指摘はPHPT 043のraw diagnostic batch oracleに限定する。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
