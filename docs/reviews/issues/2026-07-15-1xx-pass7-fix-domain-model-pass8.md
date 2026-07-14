# 1xx pass-7 fix HTTP/2 / gRPC domain model review pass 8 2026-07-15

## Scope

- consolidated pass-7 finding対応後の未コミット差分
- `src/transport.c`
- `src/status_core.c`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `src/diagnostic/bench.c`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- `tests/phpt/044-terminal-status-rst-flush-failure.phpt`
- `docs/reviews/issues/2026-07-15-1xx-adversarial-consolidated-pass7.md`
- `docs/issues/open/2026-07-10-informational-1xx-response-handling.md`
- response exchange / protocol classification / code-reading関連docs

## Reviewer Role

- HTTP/2 / gRPC domain model gate reviewer (pass 8)

## Review Prompt Summary

- pass-7の2件を対象に、terminal status-field gateがqueueした`RST_STREAM(CANCEL)`のflush失敗時に、gRPC Callのprimary status/details ownerとHTTP/2 Connectionのsecondary I/O diagnosticが分離されているかを確認した。
- queued CANCELからconnection dead / cache evictionまでのlifecycle、unary / server streaming間の整合、test-only fault seamのproduction boundary、production / diagnostic parity、PHPT 043のdeadline-aware receive経路を横断確認した。

## Review Verification

- `grpc_lite_status_code_from_call()`は`invalid_grpc_status`をconnection failureより先に`UNKNOWN`へ分類し、`grpc_lite_status_details_from_call()`も`code == GRPC_STATUS_UNKNOWN && invalid_grpc_status`をsecondaryな`grpc_message` / GOAWAY / call-local I/O snapshot / connection detail / HTTP fallbackより先に評価する。これによりpublic code/detailsは同じmalformed status lifecycleを説明し、flush失敗の`EPIPE`は`last_io_errno` / `last_io_error_detail`とconnection stateに診断情報として残る。
- test-only seamは`grpc_protocol_enforce_terminal_initial_status_fields()`によるCANCEL submit後、`nghttp2_session_send()`がqueued frameを処理し、`write_buffer_len > 0`で実際のcoalesced wire bytesが存在する場合だけ、socket flush境界で`EPIPE`を発生させる。既存のsend failure共通tailがconnectionをdeadにしてcallへI/O detailをsnapshotするため、stream-local classificationをconnection failureへ上書きせず、wire/session不一致となったconnectionだけを再利用禁止にする責務分離になっている。
- unaryはdead connectionをwrapper側でcacheから除去し、server streamingはstatus result構築後の`clear_connection_server_streaming_call_state_owner()`でdead entryをdetachしてowner countを解放する。PHPT 044は3 status fieldについてunary / server streaming双方の`UNKNOWN` + `invalid grpc-status trailer`を固定し、各isolated channelのfollow-upがfresh connectionで成功することを12本のconnection prefaceで検証するため、dead connection再駆動とcache残留のfailure modeを閉じている。
- seam predicate/stateは既存の`PHP_GRPC_LITE_ENABLE_TEST_FAULT`境界に従い、production buildでは`grpc_lite_test_fault_enabled(...)`がconstant falseへcompile outされる。新しいPHP API、runtime transport option、環境変数によるproduction transport切替は追加していない。
- production wrapperとbench-build persistent diagnosticは共通の`send_pending_h2_frames_with_deadline()`、status resolver、unary/server-streaming orchestrationを使う。raw batch diagnosticはpublic status detailsを構築しない別diagnostic surfaceだが、terminal phase/CANCEL classificationは従来どおりshared helperを使い、今回の変更でsemantic parityを分岐させていない。
- PHPT 043は`timeout_us > 0`のguard対象だけ`poll_loop=true`を渡すため、silent status-field / header-budget / pushed-stream controlsはdeadline-awareな`drive_stream_poll()`を通る。正常時は従来の`failed=1` / `timed_out=false` / exact stream errorを維持し、terminal actionが退行してpeerがsilentになった場合は約2秒で`timed_out=true`へ収束してassertion failureとなる。timeoutなしのbaselineは既存blocking diagnostic pathを維持する。
- status/metadata/phase stateはgRPC Call / HTTP/2 Stream scope、send failureとdead/cache lifecycleはHTTP/2 Connection scopeに留まり、Channel identityや別streamのsemantic stateへ新しい責務を持ち込んでいない。最終verification記録を照合し、`./tools/test/check-phpt.sh` 29/29、C unit 4/4、PHPUnit 31 tests / 116 assertions、production / bench-enabled static analysisがすべてPASSしている。意図的にsilentなpeerへのraw diagnostic probeも約2.007秒で`timed_out=true`へ収束している。`git diff --check`とuntracked PHPT 044のwhitespace checkもPASSした。

## Issues

### Blocker

none

### High

none

### Medium

none

### Low

none

### Design Decision

none

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
