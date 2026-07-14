# PR #29 第七パス C lifetime / regression-oracle review 2026-07-13

## Scope

- `3081608..199bf01`（対応commit `24abba3`、review record commit `199bf01`）
- `tests/phpt/041-fatal-mem-recv-diagnostic-caller-lifetime.phpt`
- `src/unary_call.c` の `nghttp2_session_mem_recv()` fatal cleanupとdiagnostic caller contract
- `src/transport.c` のresponse-size policy callback、persistent cache detach、connection owner / destroy lifecycle
- `src/diagnostic/bench.c` の `grpc_lite_unary()` callerとbench connection key
- `tools/test/check-c-sanitizer.sh` のASan/UBSan設定

## Reviewer Role

- `low-level C lifetime / regression-oracle adversary`

## Review Prompt Summary

- PHPT 041がoversized unary responseから`rst-submit-fatal`を経て、意図した`nghttp2_session_mem_recv()` fatal branchへ決定的に到達するか確認した。
- calleeがconnectionをconsumeするFAILURE契約について、diagnostic callerのpost-failure UAF、persistent cache entry残留、dead connection再利用、connection object破棄漏れをそれぞれoracleが検出できるか確認した。
- 132回のattempt、authorityによるdistinct key、`wire.connection_preface`件数、wireへRST_STREAMが出ないことのassertionを、cache上限とtrace実装に照合した。

## Issues

### REVIEW-20260713-004: PHPT 041はdetach後のconnection破棄漏れを検出しない

- Severity: `Low`
- Status: `Open`
- Reviewer role: `low-level C lifetime / regression-oracle adversary`
- Finding: PHPT 041のコメントと期待contractはmem-recv fatal時の「detach + destroy」まで含むが、observable oracleが検査するのは、(1) 期待exception、(2) cache entryが128件まで蓄積しないこと、(3) connectionを再利用せず132回prefaceを送ること、(4) faultで失敗したRST_STREAMがwireへ出ないこと、だけである。`src/unary_call.c`のmem-recv fatal branchから最後の`destroy_detached_connection_if_unowned(connection)`だけを削除しても、entryは既にdetachされowner countも0になるため、130 distinct-key sweepはcache exhaustionを起こさず、各attemptは新規connectionを作ってpreface件数も132のままになる。ASan laneは`detect_leaks=0`なので、detached / unownedになった132個のconnection objectは検出されない。
- Evidence: `tests/phpt/041-fatal-mem-recv-diagnostic-caller-lifetime.phpt:31-95`（detach / destroyをcontractとして記述する一方、exception・cache sweep・preface・RSTだけをassert）、`src/unary_call.c:226-237`（detach、owner clear、cleanup、destroyが独立した順序）、`src/transport.c:218-222`（実際のfreeは`destroy_detached_connection_if_unowned`だけ）、`tools/test/check-c-sanitizer.sh:33`（`ASAN_OPTIONS=detect_leaks=0`）。
- Expected model: `FAILURE consumes the connection`はcache ownershipの解除だけでなく、最後のstream ownerが消えたdetached connectionのfd、nghttp2 session、callbacks、scratch/write buffer、connection allocationを解放することまで含む。回帰testが「destroy」を保証すると記述するなら、そのterminal releaseも外部oracleで区別できる必要がある。
- Why it matters: 現実装は正しくdestroyしているが、この1行の回帰はPHPT 041とASan/UBSan gateを通過し、mem-recv fatalごとにconnection、socket fd、nghttp2 sessionとbufferを永久に漏らす。短命なPHPT processでは表面化しにくい一方、対象ユースケースである長寿命FrankenPHP workerでは反復failureによりmemory / fd exhaustionへ進む。
- Recommended fix: existing traceに、実際の`destroy_h2_connection()`実行を示すnon-secretなconnection destroy event（またはbench+test-fault build限定のdestroy counter）を追加し、PHPT 041で132個のcreated connectionすべてが破棄されたことをassertする。代替として、PHP本体由来leakをsuppressできるfocused LSan processを用意し、041相当の実行終了時にconnection allocation leakがないことを検査する。destroyをtest contractに含めない判断ならコメントをdetach / non-retentionだけへ狭められるが、長寿命workerのresource safetyは別oracleで固定すべきである。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: static mutation analysisでは`src/unary_call.c:235`のdestroy callだけを除去しても、041のPHP-level assertionに影響するstate（cache hash、new connection作成、wire preface、RST submit result）は変化しない。現HEADの実装順序自体はdetach→stream unregister / owner decrement→call cleanup→owner 0でdestroyとなっており、コード上のlifetimeは正しい。
- Notes: PHPT 041はそれ以外の目的には有効である。1024-byte `BenchReply`は8-byte上限を超え、direct parserが`response_message_too_large`を立ててcallback内RSTをsubmitし、fault seamがfatal→`NGHTTP2_ERR_CALLBACK_FAILURE`→`nghttp2_session_mem_recv()` errorへ伝播する。authorityはbench connection identityに含まれるため130件はdistinct keyとなり、detachを欠けば128件上限で期待exception assertionが失敗する。現実装がconnectionをfreeした後のdiagnostic caller dereferenceはASanが検出する。132 prefaceのassertionは「新規connectionを毎回作った」ことは示すが、旧connectionの破棄までは示さない。

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: 1 (Open)
- Design Decision: none

## Verification Notes

- `git diff --check 3081608..199bf01`: PASS。
- intended pathのstatic trace: `BenchUnary(payload_bytes=1024)` → gRPC response payload length `> 8` → `grpc_protocol_process_response_data_direct()`のmessage-too-large branch → `grpc_protocol_submit_rst_stream_in_callback()`の`rst-submit-fatal` → `NGHTTP2_ERR_CALLBACK_FAILURE` → unary coreの`nghttp2_session_mem_recv failed` FAILURE branch。
- cache oracle: persistent cache上限は128。最初のdefault-authority keyが残留した状態で130 distinct authority keyを追加すればlimitへ到達し、期待するmem-recv exceptionとの照合でdetach欠落を検出する。
- trace oracle: `wire.connection_preface`はclient connection preface送出時に1回記録され、132件は各attemptがfresh connectionだったことを検査する。fault seamはRST submit前にfatalを返すため、outbound `RST_STREAM` 0件の期待も整合する。

