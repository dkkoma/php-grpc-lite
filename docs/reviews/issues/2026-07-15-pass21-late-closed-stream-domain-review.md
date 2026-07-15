# pass-21 late closed-stream domain model review 2026-07-15

## Scope

- HEAD `2c9a61e` とcurrent working treeのpass-21未コミット差分
- `src/transport_core.[ch]`
- `src/transport.[ch]`
- `src/diagnostic/bench.c`
- `src/diagnostic/bench_call.h`
- `poc/test-server/main.go`
- `tests/unit/test_transport_core.c`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- 関連SPEC、design、code-reading、fixture、verification資料
- `docs/issues/open/2026-07-10-informational-1xx-response-handling.md`
- `docs/reviews/issues/2026-07-15-1xx-adversarial-consolidated-pass21.md`

## Reviewer Role

- HTTP/2 / gRPC domain model gate reviewer（pass-21 late closed-stream lifecycle）

## Review Prompt Summary

- valid terminal response後にclose / unregisterされたstreamへlate incomplete HEADERSが到着するcaseについて、field-class × call phaseではなくconnection-globalなinbound HPACK lifecycleとしてmodel化されているか確認した。
- productionのconnection admission / bounded flush / dead遷移、完了済みcallのtaxonomyとRST ownership、unary / server streaming、persistent reuseを確認した。
- shared pure predicateとdiagnostic sessionのsticky state、same-write raw fixture、PHPT 042 / 043、C unit、docs / bookkeepingの責務境界と識別力を確認した。

## Issues

### Blocker

- none

### High

- none

### Medium

- none

### Low

- none

### Design Decision

- none

## Domain Gate Checks

- Domain ownership: field-class × semantic phase tableはlive `grpc_call`へ公開またはreject通知されたfieldのownerとして維持され、no-live-callのsynthetic rowを追加していない。close / unregister後またはforeign streamのincomplete HEADERSは、`grpc_inbound_header_frame_requires_connection_terminal()` とproduction / diagnosticの`on_begin_frame`がHTTP/2 connection / session lifecycleとして所有する。
- Callback boundary: nghttp2 v1.69.0ではinitial HEADERSの`on_begin_frame`が`NGHTTP2_ERR_IGN_HEADER_BLOCK`判定前に発火する。same-`mem_recv`のexact local probeでもterminal trailer、stream close / user-data unregister、late HEADERSの順に進み、late frameではstream user dataが`NULL`のまま`on_begin_frame`だけが発火することを確認済みである。ignored CONTINUATIONでfield callbackが抑止されても、最初のHEADERSでEND_HEADERS欠落を確定できる。
- Pure classification: `grpc_inbound_header_frame_requires_connection_terminal(is_headers, end_headers, has_live_call)` はside effectを持たず、HEADERSかつEND_HEADERSなし、かつlive callなしの場合だけtrueになる。C unitは3入力の全8組をtable-driveし、active streamの合法なfragmented HEADERS、complete unowned HEADERS、非HEADERS frameをterminal化しない。
- Connection lifecycle: production callbackは完了済みcall、`current_read_call`、siblingへlate frameを帰属させず、RSTをsubmitせず、`draining` / `close_after_pending_flush`だけを設定する。`nghttp2_session_mem_recv()`復帰後の既存ownerがbounded pending-control flushを行いconnectionをdeadへ移すため、新規admissionを止めつつcallback内でsend / detach / destroyを行わない。
- Error taxonomy / RST ownership: late frameは既に成功完了したcallの`grpc-status: 0`、response metadata、stream error stateを変更しない。closed streamへclient-owned RSTを追加せず、connection-global HPACK incompletenessだけをtransport failureとして扱う。PHPT 042はtarget callのexact OKとoutbound RST 0件を固定する。
- Persistent reuse: same-write target処理中にconnection terminal markerが立つためtarget cleanupでdead connectionが破棄され、同じclientのfollow-upはfresh prefaceで有限にOKとなる。unaryとserver streamingの両方でtargetの成功、terminal connection destroy、fresh follow-upを別traceで確認する構造である。
- Production / diagnostic parity: production / raw diagnosticは同じpure predicateを共有し、consumerだけを各scopeに分ける。diagnosticの`bench.connection_header_block_incomplete`はcall-local reset対象ではないsession-lifetime markerとして明記され、次iterationのrequest submit前にbatchを止める。`submitted=1`、`ok=1`、`failed=1`、`timed_out=false`、実fd nonblockingをPHPT 043が固定し、完了済みiterationとpoisoned sessionを混同しない。
- Fixture / oracle: `writeRawGrpcOKThenLateIncompleteHeaders()` は103、final 200、valid gRPC DATA、`grpc-status: 0` trailer、late END_HEADERSなしHEADERSを1回の`conn.Write()`へまとめる。exact `:foo: v` HPACK fragmentとCONTINUATION省略により、stream closeとlate frameが同じ`mem_recv`へ入り得るfinding固有の順序を再現する。
- Documentation / bookkeeping: SPEC、exchange-state、protocol-classification boundary、code-reading guide、fixture catalog、verification matrixはcall-local field routingとconnection/session lifecycleを分離したcurrent modelへ揃っている。pass-21 review recordは`Status: Fixed`、Fix summary、Verification、`Fix commit: pending`を持ち、issueのProgress / Verification / Decision Logも日本語で追記済みである。
- Public / internal boundary: pure predicate、callbacks、diagnostic marker / countersはextension private C surfaceまたはbench-enabled resultに限定され、`Grpc\\` public API、channel option、transport selectionを増やしていない。flow-control、deadline、metadata/status priorityにも変更を広げていない。

## Verification

- current diff、adjacent callback registration、stream unregister、connection admission / bounded flush / dead transitionを静的照合
- production / diagnosticのlive-call判定、sticky lifetime、iteration submit gate、status非汚染を静的照合
- same-write raw fixtureとPHPT 042 / 043のunary / server streaming / diagnostic oracleを静的照合
- nghttp2 v1.69.0 sourceとexact local probeで、closed client-local streamのHEADERSでも`on_begin_frame`がignore判定前に発火することを確認
- `git diff --check` PASS
- pass-21 bookkeepingに記録されたtest-server image rebuild / force recreate PASSを照合
- pass-21 bookkeepingに記録された`./tools/test/check-phpt.sh` PASS（29/29 tests、failed 0、skipped 0、warned 0）を照合
- pass-21 bookkeepingに記録された`./tools/test/check-c-unit.sh` PASS（protocol_core / response_header_phase / status_core / transport_core、4/4群）を照合
- pass-21 bookkeepingに記録された`docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist` PASS（31 tests / 116 assertions）を照合
- pass-21 bookkeepingに記録された`./tools/test/check-c-static-analysis.sh` PASS（production / bench-enabled、findings none）を照合
- 2026-07-16: routine QA finalizationでcurrent diffとpass-21 bookkeepingを再照合し、test-server rebuild / force recreate、PHPT 29/29、C unit 4/4群、PHPUnit 31 tests / 116 assertions、production / bench-enabled static analysis findings noneを確認した。domain ownership / lifecycle modelに追加変更はなく、Blocker / High / Medium / Low / Design Decisionすべてnoneを維持。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
