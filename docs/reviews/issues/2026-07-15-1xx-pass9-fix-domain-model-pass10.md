# 1xx pass-9 fix HTTP/2 / gRPC domain model review pass 10 2026-07-15

## Scope

- pass-9 finding対応後のcurrent working tree diff全体
- `src/response_header_phase.[ch]`
- `src/grpc_exchange_state.h`
- `src/transport.[ch]`
- `src/diagnostic/bench.c`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `poc/test-server/main.go`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- `tests/unit/test_response_header_phase.c`
- pass-9 review record、仕様issue、response exchange / protocol classification / code-reading / verification current-state docs

## Reviewer Role

- HTTP/2 / gRPC domain model gate reviewer（pass 10）

## Review Prompt Summary

- response header semantic phase、最初のterminal status field transition、END_HEADERS完了前の即時CANCEL、未完了inbound HPACK blockを持つconnectionのlifecycle、Trailers-Only metadata retaggingを確認した。
- Call / HTTP/2 Stream / HTTP/2 Connectionのscope、production / raw diagnostic共有helper、unary / server streamingの終了条件、fixture / test oracle、current-state docsを横断確認した。
- repository policyに従いfull suite / Dockerは実行せず、コード、差分、既存test oracleを静的に確認した。

## Review Verification

- `grpc_response_header_phase_state.trailers_only_candidate` はblock-localで、begin / end / reset時にfalseへ戻る。END_STREAM付きFINAL_INITIALで3種類のstatus fieldが共有するone-shot transitionとなり、production / diagnostic共通observerが先行metadataをtrailingへ移し、後続fieldも同じpredicateでtrailingへ分類する。message-only / details-onlyのunary / server streaming probeとpure C unitはfield kind・順序・call iterationを跨ぐstate漏れを識別する。
- `grpc_protocol_observe_response_status_field()` は最初のEND_STREAMなしFINAL_INITIAL status fieldで `invalid_grpc_status` / body discardを確定し、header callback内でmain streamへ `RST_STREAM(CANCEL)` をsubmitする。unary / server streaming / raw diagnosticは `nghttp2_session_mem_recv()` 後にpending sendをdriveするため、対象streamはmissing CONTINUATIONの追加recv前にlocal closeへ進める。PHPT 042 / 043の3-field controlsは旧frame-end gateへの退行をexact CANCELで識別する。
- END_HEADERSなしの場合は `close_after_pending_flush` と `draining` で新規stream admissionを止める。unary / server streamingは `nghttp2_session_want_write()` だけに依存せずこのstateを見てpending sendをdriveし、coalesced wire bufferのflush成功後にconnectionをdeadへ移す。flush失敗も既存dead pathへ収束し、どちらの経路でも以後session / socket I/Oを再駆動しない。
- 対象callはflush成功時に追加のconnection failureを付けず、flush失敗時も `invalid_grpc_status` がstatus / detailsのprimary ownerとなるため `UNKNOWN + invalid grpc-status trailer` を維持する。既存siblingはunary / server streamingのI/O gateでconnection breakを記録して `UNAVAILABLE + incomplete HTTP/2 response header block` へ収束する。
- cache entryはsend helper内で解放せず、対象unaryのadapter cleanupまたはserver-streaming owner clearでdetachする。active siblingが残る場合は `stream_owner_count` によりconnection objectを保持し、最後のowner clearでのみ破棄するため、dead後にnghttp2 stream user dataへ触れずcall / connection lifetimeを閉じる。
- raw diagnosticはproductionと同じstatus-field observer / phase helper / RST submitを使う。raw batchにはpersistent `h2_connection` ownerがないためconnection quarantineはno-opだが、one-shot sessionをbatch終了時に破棄する境界は維持される。production / diagnosticのsemantic classificationとstream-local CANCELは一致している。
- `multiplex-hold-sibling` / `multiplex-incomplete-grpc-status` probeは同じpersistent connection上の先行server-streaming ownerと後続unary targetを作り、targetのUNKNOWN / exact CANCEL、siblingの追加I/OなしUNAVAILABLE、cache eviction後のfresh follow-upを識別する。current-state docsもGOAWAY drainingとincomplete HPACK terminal quarantineを分離している。
- 再レビューではBlocker / High / Medium / Lowに該当するconnection / stream / call scope、status precedence、owner / cache lifetime、production / diagnostic boundaryの追加不整合を検出しなかった。repository policyと依頼に従いfull suite / Dockerは実行せず、current working treeのコード、fixture、test oracle、docsを静的に照合した。

## Issues

### Blocker

- none

### High

- none

### Medium

#### REVIEW-20260715-001: 未完了HPACK blockのconnectionをdrainingに留めると既存sibling streamが停止できる

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `HTTP/2 / gRPC domain model gate reviewer（pass 10）`
- Finding: END_HEADERSなしFINAL_INITIALでterminal status fieldを観測すると、対象streamには即時CANCELをqueueする一方、connectionはGOAWAYと同じ `draining` にするだけである。`draining` は新規stream admissionを止めるが、既存active streamにはI/Oを許すmodelである。missing CONTINUATIONによりconnection-globalなinbound HPACK parserが完了不能な場合、同じconnection上の既存sibling streamは正常に完走できず、deadlineなしdrive loopがpeer inputを待ち続けられる。
- Evidence: `grpc_protocol_observe_response_status_field()` (`src/transport.c`) はEND_HEADERSなしで `mark_connection_draining(call->connection, 0, NGHTTP2_PROTOCOL_ERROR)` を呼ぶ。`connection_io_allowed()` は `draining` を拒否せず、unary / server streamingのdrive loopはdeadでないdraining connectionを継続してrecvする。`h2_connection.active_streams` / `active_stream_count` と `register_grpc_call_stream()` は同一connection上の複数ownerを表現し、`preflight_persistent_connection()` もactive streamがあれば同connectionへのstream追加を許すため、incomplete block検出時にsibling ownerが存在し得る。現在のPHPT 042は対象callのUNKNOWN、CANCEL、cache上のfresh follow-upを確認するが、先行してactiveな別streamの有限終了は確認しない。さらに `mark_connection_draining()` はlocal parser quarantineでも `last_goaway_*` を更新するため、GOAWAY drainingとdecoder同期喪失という異なるconnection lifecycle reasonを同じstateへ畳んでいる。
- Expected model: incomplete inbound HPACK blockは対象gRPC CallだけでなくHTTP/2 Connection全体の継続可能性を失わせる。対象streamのCANCELを可能ならflushした後、connectionをdead / close-after-flushへ移してcacheから外し、既存sibling ownerも再度session I/Oをdriveせずconnection failureとして有限に終了させる必要がある。GOAWAYのdraining（admit済みstream完走可）と、decoder同期喪失によるterminal quarantine（全owner完走不可）は別state / reasonとして扱う。
- Why it matters: server streaming resourceなど別streamが同じpersistent connection上でactiveなとき、adversarial peerは一つのincomplete status blockで対象callだけでなくdeadlineなしsibling call / PHP workerも保持できる。新規follow-upがfresh connectionへ移ることだけでは、既存ownerのlivenessとcall user-data lifecycleを閉じられない。
- Recommended fix: connection-localな `close_after_pending_flush` / terminal quarantine等を導入し、header callbackではtarget RSTをqueueして新規admissionを停止し、直後のpending frame flush完了後にconnectionをdead化・cache detachする。RST submit / flush失敗時も既存dead pathへ収束させる。target callのprimary `UNKNOWN + invalid grpc-status trailer`は維持し、siblingは `UNAVAILABLE` とする。raw fixture / PHPTへ、同一connection上で先行server-streaming streamをactiveにした後に別streamへincomplete status blockを送り、targetのexact CANCELとsiblingの有限UNAVAILABLE、fresh follow-upを確認するprobeを追加する。
- Fix summary: `h2_connection` に `close_after_pending_flush` を追加し、END_HEADERS未完了のstatus field観測時はGOAWAY stateを捏造せず、新規admissionだけを即時停止するterminal quarantineを設定した。unary / server streamingはmem-recv後に同stateを明示的に見てpending `RST_STREAM(CANCEL)` をflushし、成功後にconnectionをdeadへ移す。send失敗も既存dead pathへ収束する。dead後はunaryに追加したI/O gateとserver streamingの既存gateがsession / socket再駆動を止め、targetは `invalid_grpc_status` の優先順位でUNKNOWN、siblingはconnection breakとしてUNAVAILABLEとなる。cache detachとconnection破棄は既存owner cleanupへ委ね、active siblingが残る間はowner countでlifetimeを保持する。raw fixture / PHPT 042へactive siblingと別stream targetのmultiplex probeを追加した。`
- Fix commit: `pending`
- Verification: `静的再レビューで、(1) status field callbackがtarget CANCELをqueueしてclose-after-pending-flushを設定すること、(2) unary / server streamingの両drive loopが同stateをwant_writeとは独立にflushすること、(3) flush成功後または失敗時にconnectionがdeadとなること、(4) unary / server streamingがdead後の追加I/Oを止めること、(5) target status/detailsはinvalid_grpc_statusが優先し、siblingはconnection_brokenでUNAVAILABLEとなること、(6) target cleanupがcacheをdetachしactive siblingのowner clearまでconnection破棄を遅延することを照合した。test-server rebuild / restart後の./tools/test/check-phpt.shは29/29 PASSし、PHPT 042でtarget UNKNOWN / target streamに対応するexact CANCEL / persistent reuse、sibling UNAVAILABLE / semantic details / target終了後の追加wire I/Oなし、follow-upのnew preface / persistent_reused=falseを確認した。C unit 4/4、PHPUnit 31 tests / 116 assertions、production / bench-enabled static analysisもPASS。`
- Notes: 単一active streamに対するpass-9 Mediumの即時CANCELに加え、connection-global parser stateと複数既存stream ownerのscope差もclose-after-pending-flush lifecycleで閉じた。

### Low

- none

### Design Decision

- none

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
