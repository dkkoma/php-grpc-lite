# 1xx informational response adversarial consolidated pass 9 2026-07-15

## Scope

- 対象 commit: `20c2dc0`, `6a4902f`, `0e22a8a`, `a80556f`, `6168e2e`
- 対象 branch / HEAD: `codex/issue-informational-1xx-response-handling` / `6168e2e`
- 仕様 issue: `docs/issues/open/2026-07-10-informational-1xx-response-handling.md`
- 過去 record: pass 1 / pass 3 の protocol, C safety, test review、pass 2 / pass 4 domain-model gate、pass 5 / pass 7 adversarial review、pass 6 / pass 8 domain-model gate
- 主対象: response header-block semantic phase、wire budget、terminal status gate、CANCEL flush failure の status ownership、raw HTTP/2 fixture と PHPT 042 / 043 / 044

## Reviewer Role

- consolidated adversary: protocol + C safety / lifetime + test / fixture

## Review Prompt Summary

- pass 9 の convergence check として、過去 finding の修正が stated failure mode を閉じているか、また現在の実装に新しい edge がないかを adversarial に確認した。
- 最新 increment `6168e2e` の CANCEL flush failure 時の status ownership と PHPT 044、PHPT 043 の deadline-aware `poll_loop` guard を重点確認した。
- repository policy に従い test suite / Docker は実行せず、code、diff、history、docs、fixture、既存 test oracle を静的に確認した。

## Issues

### REVIEW-20260715-001: END_HEADERS 未完了の status-field block では frame-end CANCEL へ到達せず call が停止する

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `protocol + C safety + test / fixture adversary`
- Finding: non-`END_STREAM` の `FINAL_INITIAL` block で `grpc-status`, `grpc-message`, `grpc-status-details-bin` のいずれかを受信すると、`on_header_callback()` は semantic failure を確定して `invalid_grpc_status` を保存する。しかし stream-local CANCEL は header block 全体の完了後に呼ばれる `on_frame_recv_callback()` 内の `grpc_protocol_enforce_terminal_initial_status_fields()` でしか queue されない。HEADERS が `END_HEADERS` を持たず、peer が必要な CONTINUATION を送らず connection を開いたままにすると、失敗を既に認識しているのに CANCEL を送れず、deadline のない call は終了しない。
- Evidence:
  - `src/transport.c` の `on_header_callback()` は nonterminal `grpc-status` を見た時点で `grpc_call_set_invalid_grpc_status()` と `grpc_call_discard_response()` を実行するが、その場では `grpc_transport_session_queue_rst_stream()` を呼ばない。`grpc-message` / `grpc-status-details-bin` も同じ terminal-status gate を通る。
  - CANCEL queue は `on_frame_recv_callback()` が `grpc_protocol_enforce_terminal_initial_status_fields()` を呼ぶ経路にある。nghttp2 の callback contract では、HEADERS と後続 CONTINUATION は一つの header block として扱われ、全 header field の callback が成功して block が完了した後に `on_frame_recv_callback()` が呼ばれる。そのため CONTINUATION が欠落すると frame-end gate は実行されない。
  - `src/unary_call.c` と `src/server_streaming_call.c` の drive loop は、`invalid_grpc_status` の保存だけでは抜けず、stream close、I/O failure、または deadline を待つ。deadline が設定されていない場合、この sequence は peer が TCP connection を閉じるまで停止できる。
  - exact runtime probe: 同一 stream に `HEADERS(:status: 103, END_HEADERS)`、続いて `HEADERS(:status: 200, content-type: application/grpc, grpc-status: 0, END_STREAM=0, END_HEADERS=0)` を送り、必要な CONTINUATION を送らず TCP connection を開いたままにする。HPACK fragment は listed fields の header callback が発火できる長さまで含める。`grpc-message` または `grpc-status-details-bin` に置き換えた variant も同じ failure mode を確認する。期待 oracle は有限時間内の failure と、少なくとも stream-local CANCEL を試みたことの観測である。
  - 現在の raw HTTP/2 fixture の `writeRawHeaders()` は常に `EndHeaders: true` を設定するため、PHPT 042 / 043 / 044 はこの sequence を生成せず、pre-fix behavior を識別できない。
- Expected model: non-`END_STREAM` の `FINAL_INITIAL` block に terminal status field が現れた時点で、その response は正常な gRPC response へ回復できない。semantic failure の transport action は header-block completion のみに依存せず、call を silent peer から切り離せる必要がある。残りの block から後に判明する HTTP/2 protocol error の precedence と、未完了 HPACK block を持つ connection の再利用可否は別途一貫して処理する。
- Why it matters: malformed または adversarial peer が、php-grpc-lite に semantic failure を認識させた後も、deadline のない unary / server-streaming call と PHP worker を保持できる。pass 5 の terminal status gate が意図した「silent peer を待たない」という failure mode closure が、fragmented header block では成立していない。
- Recommended fix: terminal status field の header callback で response-level failure を確定した時点に、header-block completion を待たず stream cancellation / call termination を開始する。nghttp2 が未完了 header block を保持した connection を安全に継続できない場合は connection を draining / dead とするなど、reuse policy も明示する。上記 exact sequence を raw fixture へ追加し、有限時間内の終了、status precedence、RST_STREAM または connection shutdown の wire oracle を固定する。
- Fix summary: 3種類のstatus fieldが最初に観測されたfield callbackから呼ぶproduction / diagnostic共有の `grpc_protocol_observe_response_status_field()` を追加した。END_STREAMなしFINAL_INITIALでは同callback内で `invalid_grpc_status` / body discardを確定し、frame-endを待たずmain streamへ `RST_STREAM(CANCEL)` をsubmitする。END_HEADERS未完了blockではinbound HPACK stateを再利用できないためconnectionへclose-after-pending-flush terminal quarantineを設定し、現在のCANCELをflushした後にdead化する。対象callは`UNKNOWN`を維持し、既存siblingはsession / socketを再駆動せず`UNAVAILABLE`、follow-upはfresh connectionとなる。raw fixtureは3 fieldそれぞれのCONTINUATION欠落と、同一connection上にactive server-streaming siblingを置くmultiplex controlを持ち、PHPT 042でdeadlineなしunary / server streamingのUNKNOWN、exact CANCEL、siblingの有限UNAVAILABLE / 追加I/Oなし、fresh follow-up、PHPT 043でdiagnostic failed-not-timedout / CANCELを固定した。
- Fix commit: `pending`
- Verification: `test-serverをrebuild / restart後、./tools/test/check-phpt.sh PASS（29/29）。PHPT 042は6個のdeadlineなしcallがUNKNOWN + invalid grpc-status trailerへ収束し、各targetのexact RST_STREAM(CANCEL)とfresh follow-upを確認した。multiplex probeは対象callのUNKNOWN、target streamに対応するexact CANCEL、既存siblingの追加wire I/OなしUNAVAILABLE、次RPCのfresh connectionを確認した。PHPT 043は3 fieldともfailed=1 / timed_out=false / stream_error_code=8を確認した。./tools/test/check-c-unit.sh PASS（4/4）、PHPUnit PASS（31 tests / 116 assertions）、./tools/test/check-c-static-analysis.sh PASS（findings none）。pass-10 domain model再レビューはBlocker / High / Medium / Low / Design Decisionすべてnone。`
- Notes: pass 5 の `REVIEW-20260715-001` の修正が fragmented block では不十分であることを示す新規 finding。accepted Decision Log の再検討ではない。

### REVIEW-20260715-002: grpc-status-details-bin だけの terminal final block が一つの block を initial / trailing metadata に分割する

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `protocol + test / fixture adversary`
- Finding: terminal `FINAL_INITIAL` block に `grpc-status-details-bin` があり `grpc-status` がない場合、当該 field だけが trailing metadata に分類され、その前後の application metadata は initial metadata に残る。`grpc-message` だけの場合も status detail は保存されるが、同じ block の metadata role は initial のままである。header-block semantic phase model にもかかわらず、一つの terminal final block の role が field kind と field order で分裂する。
- Evidence:
  - `src/response_header_phase.c` の `grpc_response_header_phase_metadata_is_trailing()` は `TRAILING` phase または `grpc_status_seen` のみを trailing 判定に使う。
  - `src/transport.c` の terminal `grpc-status-details-bin` branch は、その field の local `trailing` flag だけを `true` にする。既に initial metadata に追加された field を trailing へ移動せず、`grpc_status_seen` も更新しないため、その後の metadata は再び initial に分類される。`grpc-message` branch も prior / subsequent metadata の role を変更しない。
  - exact runtime probe: `HEADERS(:status: 103, END_HEADERS)` の後、`HEADERS(END_HEADERS|END_STREAM)` に `:status: 200`, `content-type: application/grpc`, `x-before: a`, `grpc-status-details-bin: AA==`, `x-after: b` をこの順で入れ、`grpc-status` を送らない。現状は `x-before` と `x-after` が initial、`grpc-status-details-bin` だけが trailing になる。`grpc-message: detail` だけを置く variant も確認対象になる。missing `grpc-status` による final code `UNKNOWN` は本 finding の対象外である。
  - `src/diagnostic/bench.c` も同じ `grpc_status_seen` predicate と field-local details-bin classification を複製しており、production / diagnostic classification は同じ不整合を持つ。
  - PHPT 042 の invalid status metadata probe は invalid `grpc-status` を使い、PHPT 022 の message-only probe は initial block とは別の trailing block を使うため、この Trailers-Only candidate を識別しない。
  - `docs/SPEC.md` の Headers-Only / Trailers-Only model は、final terminal block の custom metadata と status metadata を同じ trailing role とし、parse / field order に依存しない分類を要求している。
- Expected model: 一つの completed response header block は一つの semantic role を持つべきである。terminal `FINAL_INITIAL` block で status field の存在により Trailers-Only candidate と判定するなら、三つの status field のどれが最初に現れても prior metadata を trailing へ移し、subsequent metadata も trailing に保つ。missing `grpc-status` の final status taxonomy は metadata ownership と独立して扱う。
- Why it matters: application metadata の observable ownership が header order と status field の種類で変わり、同じ terminal header block が initial / trailing に分裂する。interceptor や client code が initial / trailing metadata を別用途で扱う場合に互換性差となる。
- Recommended fix: block-local な `any terminal status field seen` または Trailers-Only candidate state を `grpc-status`, `grpc-message`, `grpc-status-details-bin` の全てで共有し、最初の terminal status field で prior metadata を trailing へ移したうえで、その後の field も trailing に分類する。production / bench の共通 helper 化または同一 transition の C unit を追加し、details-only / message-only の前後に custom metadata を置く unary / server-streaming probe を追加する。
- Fix summary: `grpc_response_header_phase_state` にblock-localな `trailers_only_candidate` を追加した。END_STREAM付きFINAL_INITIALで `grpc-status` / `grpc-message` / `grpc-status-details-bin` のいずれかを最初に観測した時に共有phase helperがcandidateへ遷移し、production共有observerが先行metadataをtrailingへ移す。後続fieldも同じpredicateでtrailing ownershipとなり、field kind・parse成否・順序によるblock分割をなくした。production / diagnosticの全3 branchを同じhelperへ接続し、C unitでtransition / end / reset、PHPT 042でmessage-only / details-only blockのbefore-after metadata ownershipをunary / server streaming双方に固定した。
- Fix commit: `pending`
- Verification: `./tools/test/check-c-unit.sh PASS（4/4）。response header phase suiteでblock-local candidateのone-shot transition、metadata role、end/resetを確認した。test-serverをrebuild / restart後の./tools/test/check-phpt.sh PASS（29/29）。PHPT 042のunary / server streamingでmessage-only / details-only terminal final blockのx-before / x-afterがinitialに存在せずtrailingへ揃うこと、details-bin自体がraw binary trailing metadataになることを確認した。production / diagnosticの全3 field branchが同じobserver / phase predicateを使うことを静的照合した。PHPUnit PASS（31 tests / 116 assertions）、./tools/test/check-c-static-analysis.sh PASS（findings none）。`
- Notes: pass 1 protocol `REVIEW-20260715-004` と同種の metadata ownership invariant を、現在の三-field terminal gate に対して確認した新規 finding。accepted Decision Log の再検討ではない。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
