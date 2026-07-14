# PR #29 deadline RST_STREAM connection reuse 敵対的テストレビュー 2026-07-11

## Scope

- PR #29: base `e49d4be` / second re-review HEAD `be1b97e`（fix commit `b13362b` vs `6795a5a`）
- 元issue: `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md`
- `src/transport.c` (`cancel_grpc_call_stream`, `drain_pending_connection_data_for_reuse`, `send_pending_h2_frames_with_deadline`, status details)
- `src/unary_call.c` のdeadline read path
- `src/server_streaming_call.c` のdeadline / concurrent stream / connection-broken path
- `src/status_core.c`, `src/grpc_exchange_state.h` のstatus taxonomy
- `tests/phpt/033-deadline-rst-stream-connection-reuse.phpt`
- `tests/phpt/034-dead-connection-terminal-for-owners.phpt`
- `tests/phpt/035-preflight-drain-cap-fallback.phpt`
- `tests/phpt/036-draining-connection-cancel-sends-rst.phpt`
- `poc/test-server/main.go` の `BenchUnary` / `BenchServerStream` / ports 50066〜50069 raw/control fixtures
- PHPT runner / coverage / sanitizer / ZTS port preflight scripts

## Reviewer Role

- `adversarial test / compatibility reviewer`

## Review Prompt Summary

- 元issueの範囲内で、unary / server streaming deadline時の正しいstreamへの `RST_STREAM(CANCEL)`、connection reuse、deadlineなし並行streamの生存、setup deadline非漏洩、RST flush失敗時のconnection破棄、status/error details非漏洩を反証する。fixtureの実際の送信時刻、trace assertionの独立性、異常分岐、到着済み／遅延frame、TLS経路を確認する。

## Adversarial Checks

- 現行PHPT 033単体: PASS (1/1、約0.95秒)。
- TLS (`test-server:50052`) のunary timeout / streaming timeout / 各follow-up reuse: `DEADLINE_EXCEEDED`, `OK`, `RST_STREAM(CANCEL)` 2本を確認。
- timeout後も実際にI/Oが残るsurvivor (`message_count=3`, `payload=131072`, `server_delay_ms=700`, HTTP/2 window=65535): 3 message / `STATUS_OK`、約1.42秒で完走。
- 到着済みDATAが64KiB未満のslow-consumer timeout: preflightで26,660 bytesを消化し、follow-upは `persistent_reused=true`。
- 到着済みDATAが64KiB以上のslow-consumer timeout、およびlarge unary timeout: 下記REVIEW-20260711-001の再現どおりconnection reuseに失敗。
- HEAD `6795a5a`: PHPT 033 / 034 / 035の同時実行は3/3 PASS。
- HEAD `6795a5a`: post-RST writeを強制するsurvivor (`payload=131072`, `delay=700ms`, stream/connection window=65535) は3 messages / STATUS_OK、RST後のstream WINDOW_UPDATE 6件、terminal HEADERS 1件を確認。
- HEAD `6795a5a`: PHPT 035をmodule rebuildなしで12回反復し、PASS 11 / FAIL 1。FAILは期待 `persistent_reused=false` に対して実値 `true`。
- HEAD `6795a5a`: port 50066相当の直接probeで、connection kill後のsurvivorは即終了し `STATUS_UNKNOWN(2)` / details `recv failed: Connection reset by peer`。unary Bは `STATUS_UNAVAILABLE(14)`。
- gRPC Coreのstatus-code tableは「connectionが切れる前にdataが送信された」client-side failureを `UNAVAILABLE` と規定する。port 50066のsurvivorはresponse DATAを1件受信済みでconnection breakも既知なのに `UNKNOWN` となるため、REVIEW-20260711-007としてAPI互換性不備を記録。
- port 50066を追加した全PHPT runner (`check-phpt`, C coverage, ASan/UBSan/TSan PHPT, ZTS PHPT) のpreflight配列を確認。Crash/UB runnerはPHPTを実行しないため追加不要。
- HEAD `be1b97e`: rebuildなしの既存 `modules/grpc.so`（INI default 65536を確認）でPHPT 034 / 035 / 036は3/3 PASS。3本を同一container内で30回反復し `PASS:30 FAIL:0`。
- HEAD `be1b97e`: PHPT 034相当の直接probeはunary B=`UNAVAILABLE(14)`、survivor A=`UNAVAILABLE(14)`、双方details=`recv failed: Connection reset by peer`。preface 1本 / server-streaming request 1 attemptでtransparent retryなし。
- HEAD `be1b97e`: PHPT 035相当のcontrol barrierは `armed` / `ready` を返し、follow-up=`STATUS_OK`, `persistent_reused=false`, preface 2本。ただしINI 16384に対しpreflight readは1回で `requested_len=65536`, `result_len=49179`（REVIEW-20260711-008）。
- HEAD `be1b97e`: PHPT 036相当のtraceはempty `BenchRequest`（serialized payload 0、wire DATAは5 bytes + END_STREAM）をGOAWAY前に完送し、streamもownerも1本だけ。その後のRST_STREAM(CANCEL)は確認したがpending DATA provider UAF lifecycleは作らない（REVIEW-20260711-009）。
- HEAD `be1b97e`: port 50069の2回目の `arm` で、1 message受信済みのport 50068 stream自身が次pull中にdirect EOFとなるprobeを実施。結果は `UNKNOWN(2)` / details=`connection closed`、preface 1本 / request 1 attemptでretryなし（REVIEW-20260711-010）。fixtureのarmed stateはsacrificial h2 prefaceで消費して復元。
- ports 50067〜50069はPHPTを実行する全runner (`check-phpt`, C coverage, ASan/UBSan/TSan PHPT, ZTS PHPT) のpreflightへ追加済み。MSanはC unitのみでPHPT preflight前に終了するため追加経路不要。
- HEAD `be1b97e`: existing moduleをrebuildせずfull Docker PHPTを実行し21/21 PASS、skipped/warned/failedすべて0。run-tests計測3.505秒、shell wall 3.59秒（user 0.26 / sys 0.18秒）。
- HEAD `be1b97e`: full PHPT成功後にC unitを実行し、`protocol_core` / `status_core` / `transport_core` の3/3 PASS。wall 1.12秒（user 0.06 / sys 0.02秒）。

## Issues

### REVIEW-20260711-001: 64KiB以上の到着済みresponse DATAがあるdeadline cancelではpreflight上限に達し、RST成功後もconnectionを再利用できない

- Severity: `Medium`
- Status: `Accepted`
- Reviewer role: `adversarial test / compatibility reviewer`
- Finding: `cancel_grpc_call_stream()` が `RST_STREAM(CANCEL)` を正常送出しても、deadline時点でsocketに64KiB以上のresponse DATAが到着済みだと、次callの `drain_pending_connection_data_for_reuse()` は1回の64KiB readで `GRPC_LITE_PREFLIGHT_DRAIN_MAX_BYTES` に達する。read boundary (`EAGAIN`) を確認できないままconnectionをdraining扱いにし、follow-upは新しいTCP connectionを作る。これはno-response-backlogのPHPT 033では見えず、PRのunary / server streaming双方の「deadline後にpersistent connectionを温存して後続callで再利用する」という無条件の記述を満たさない。
- Evidence: `src/transport.c:15-16,902-988`。server streaming再現は `message_count=1000`, `payload_bytes=65536`, `timeout=300ms`、最初のyield後500ms sleepで、`delivered=1`, `stream_status=DEADLINE_EXCEEDED`, outbound RST=1、その後のunaryはOKだが `persistent_reused=false`, `wire.connection_preface=2`, `wire.socket_preflight_read=65536`。unary再現はconnectionをprime後、`payload_bytes=67108864`, `timeout=20ms`, max receive=128MiBで、timeout callはRST=1、その後のunaryはOKだがreuse flagsが `[false,true,false]`, preface=2, preflight read=65536。対照として26,660 bytesのbacklogでは `persistent_reused=true`。
- Expected model: read側deadline expiryでRSTのflushに成功し、nghttp2がcancel済みstreamの到着済みframeを安全に処理できるなら、通常のHTTP/2 receive window内のbacklogだけを理由にconnectionを捨てず後続callへ再利用する。resource上限によるfallbackを意図的に残すなら、SPEC / issue / close criteriaは「backlogがpreflight上限未満の場合」に保証を限定し、official実装との差を明示する。
- Why it matters: large unary responseやserver streamingのslow consumerは本番で自然に発生する。まさにdeadline時のhandshake再発を避けるPRなのに、response backlogがある現実的なケースでは従来同様に再接続となり、PRの主目的と互換性説明が過大になる。RPC結果自体は新connectionで安全に回復するためHighではなくMediumとする。
- Recommended fix: cancel済みstreamのpreflightは、単一64KiB readではなく短い時間budget内でread boundaryまでnghttp2へ入力する。byte guardは少なくともconfigured connection receive windowとkernel/TLS backlogを考慮し、上限超過時のみ安全側にconnectionを破棄する。PHPTへserver-streaming slow-consumerとlarge unaryの64KiB境界ケースを追加し、RST後のfollow-upでprefaceが1本、`persistent_reused=true`となることを固定する。現行fallbackを設計判断として残す場合は保証を条件付きへ修正し、上限到達時のpreface=2を明示的にテストする。
- Fix summary: connection reuseをbest-effort policyとして採用し、SPEC §4.2へpreflight上限到達時はdraining / fresh connectionへ安全にfallbackすることを明記。実装でbounded adoptionを追加しない判断は元issueの安全なconnection fallback内で明示された。PHPT 035を追加したが、cap到達のfixture同期にはREVIEW-20260711-006のflakyが残る。
- Fix commit: `0480479`
- Verification: 既存h2c再現でserver streaming / unary双方の64KiB上限と新connection生成を確認。HEADのSPEC記述をreview。PHPT 035の単発PASSだけではcap pathを決定的に固定できず、12回中1回はreuse=trueとなったためtest安定性は別issueとしてOpen。
- Notes: `recv()` がちょうど64KiB返しただけでは、そのreadでbacklogを読み切ったかも判定できない。現実装は追加peekもせず必ずdrainingへ落とすため、境界値そのものにも保守的なfalse negativeがある。

### REVIEW-20260711-002: 「並行deadlineなしstreamが生存」のPHPTはsurvivorがRSTより前にwire上で完了しており、timeout後の生存を検証していない

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `adversarial test / compatibility reviewer`
- Finding: PHPT 033のsurvivorは `message_count=3`, `server_delay_ms=100` で、Go fixtureはmessageを0ms / 100ms / 200msに送る。一方、並行unaryのdeadlineは300msである。unary receive loopが同じconnectionのsurvivor frameもdispatchするため、survivorはPHP側のqueueに残って見かけ上in-flightでも、wire上ではtimeout RST前にtrailersまで受信してstream close済みになる。したがってassertion名の「in-flight stream survived concurrent deadline expiry」は、timeout後のsurvivor read、WINDOW_UPDATE、期限なしwrite、setup deadline非漏洩のいずれも通していない。
- Evidence: `tests/phpt/033-deadline-rst-stream-connection-reuse.phpt:62-81`、`poc/test-server/main.go:356-375`。同条件のtraceではsurvivor stream 1のterminal HEADERSがmonotonic `1883541153229`、timeout unary stream 3のRSTが `1883541259213` で、survivor完了がRSTより約106ms早かった。fixtureの送信時刻からも決定的に同じ順序になる。
- Expected model: unrelatedなdeadlineなしstreamの少なくとも1つのresponse frameとそれに伴うnghttp2 write処理が、他streamのdeadline RSTより後に発生し、それでも同じconnection上で全message / trailers / `STATUS_OK`まで完了することをtestが証明する。
- Why it matters: `setup_deadline_abs_us` のscope修正は並行streamを巻き込まないために入ったHigh修正だが、現在の回帰testはその失敗条件を通らない。将来、connection-scopedな期限切れdeadlineを再導入しても、queued responseを数えるだけの現行assertionはPASSし得る。
- Recommended fix: survivorを例えば `message_count=3`, `server_delay_ms=700` とし、300ms timeoutより後に2, 3個目が到着するようにする。小さいDATAでWINDOW_UPDATEが出ない偽陰性を避けるため、test限定でreceive windowを65535、payloadを131072程度にして、survivorのterminal HEADERS時刻がtimeout RSTより後であること、survivor宛RSTがないこと、全message / `STATUS_OK`をassertする。
- Fix summary: committed PHPT 033のsurvivor delayを700msへ延長。survivor stream IDをtraceから特定し、survivor宛RSTなし、terminal HEADERSが並行unary RSTより後、全message / STATUS_OKをassertするよう修正。
- Fix commit: `0480479`
- Verification: PHPT 033 PASS。trace ordering assertionをreview。さらにwindow=65535 / payload=131072の強化probeでRST後のstream WINDOW_UPDATE 6件とterminal HEADERSを確認し、実装のpost-RST read/writeは正常。
- Notes: sequentialなsetup deadline残留は、最初のtimeout後のdeadlineなし `SayHello` が成功する現行assertionで一定程度固定されている。本指摘は並行stream scopeの回帰防止に限定する。

### REVIEW-20260711-003: RST submit / coalesced flush失敗でconnectionを必ず殺す安全分岐に故障注入テストがない

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `adversarial test / compatibility reviewer`
- Finding: `cancel_grpc_call_stream()` は `nghttp2_submit_rst_stream()` 失敗、または50ms grace内の `send_pending_h2_frames_with_deadline()` 失敗時にconnectionをdeadへ落とす設計だが、PHPT 033は正常flushとreuse成功しか作らない。さらに `wire.frame_out` はcoalescing bufferへframeを積んだ直後、実socket flushより前に記録されるため、異常系ではtraceにRSTがあってもflush成功の証拠にならない。nghttp2 sessionだけが「sent」へ進みwireと不整合になったconnectionを再利用しない、という最重要fallbackがコード読解だけに依存している。
- Evidence: `src/transport.c:319-347` (`cancel_grpc_call_stream`), `src/transport.c:1794-1815` (`send_callback` はbuffer後にtrace), `src/transport.c:1818-1867` (後段flush失敗で `mark_connection_dead`)。`tests/phpt/033-deadline-rst-stream-connection-reuse.phpt` はRST 3本と全follow-upの `persistent_reused=true` のみで、submit error / partial or failed flush / grace timeoutを生成しない。
- Expected model: RST submitまたはflushが失敗したconnectionは即座にcacheから再採用不能となり、次callは別connection(preface追加、`persistent_reused=false`)で成功する。失敗したsessionのpending stateやconnection error detailsを後続callへ渡さない。
- Why it matters: このfallbackが壊れると、部分frameまたはnghttp2/wire state不一致のconnectionを再利用して並行callも含めてprotocol corruptionへ進む。正常系だけのtestでは、`mark_connection_dead()` の削除・順序変更・return値無視を検出できない。
- Recommended fix: production surfaceへdebug責務を漏らさず、test buildのwrite seamまたは専用raw fixtureで「RST submit後の最終flushがEPIPE/timeoutになる」ケースを決定的に注入する。unary / server streaming双方でdeadline statusを保ち、次callが新preface / `persistent_reused=false` / `STATUS_OK`となることをassertする。traceをwire evidenceとして用いるなら、buffer enqueueとsuccessful socket flushを区別するeventも検討する。
- Fix summary: `connection_io_allowed()`、server-streaming next loop guard、send pathのdead早期returnにより、dead後の再駆動をコード上は禁止。PHPT 034 / port 50066 fixtureで、別ownerがTCP EOF/ECONNRESETを観測してdead化した後のsurvivorを検証した。ただしfixtureはRST submit / cancel grace flush failure、0-byte/partial writeを生成しないため、本findingのfault branch coverageは未完了。
- Fix commit: `0480479` (partial), `b13362b` (fatal cleanup / taxonomy follow-up; fault injectionは未実装)
- Verification: HEAD `be1b97e` でPHPT 034 PASS、30回反復もflakeなし。shared-owner直接probeではsurvivorも `UNAVAILABLE(14)` へ修正された。一方、034 / 036はいずれも正常なRST/TCP closeしか作らず、RST submit NOMEM、coalesced bufferの0-byte/partial flush failure、grace timeoutは依然通らない。034の「dead後I/Oなし」oracleも `monotonic_us > unaryEndUs` 比較のため、同一microsecondの即時re-driveを見逃し得る。JSONL record indexによる順序assertionへ直す余地がある。
- Notes: 現状実装のterminal guard自体はadequate。Open理由は、明示されたRST flush failure fallbackを決定的に実行するtestが依然ないこと、および034のordering oracleが完全ではないこと。

### REVIEW-20260711-004: connection-scoped timeout errorをclearしたことをfollow-up status detailsでassertしていない

- Severity: `Low`
- Status: `Open`
- Reviewer role: `adversarial test / compatibility reviewer`
- Finding: 修正はRST flush成功時に `connection->last_error_detail`, `last_io_errno`, `last_ssl_error` をclearするが、PHPT 033はfollow-upのstatus codeとresponseだけを確認し、`$status->details` を確認しない。clearを削除してもfollow-upは`grpc-status: 0`によりcode=OKのまま、`grpc_lite_status_details_from_call()` が前callのconnection detailを先に採用してdetailsへ `HTTP/2 transport deadline exceeded` を漏らすため、現行testはPASSし得る。
- Evidence: `src/transport.c:337-346`, `src/transport.c:2229-2242`、`tests/phpt/033-deadline-rst-stream-connection-reuse.phpt:39-59,76-81,115-126`。既存 `tests/phpt/010-unary.phpt` のempty details assertionはfreshな正常callであり、timeout後の同connectionを対象にしない。
- Expected model: stream-scoped deadline detailはtimeout callにのみ属し、温存したconnection上のOKまたは別protocol failureのstatus detailsへ混入しない。
- Why it matters: codeがOKでもdetailsだけ前callのdeadlineとなると、official wrapper利用側のログ／retry診断を誤らせる。今回追加したclearの目的を直接守る回帰testがない。
- Recommended fix: unary timeout後とserver streaming timeout後の各 `SayHello` で `STATUS_OK` に加えて `details === ''` をassertする。可能なら同connection上で独自detailsを持つ失敗callも続け、前deadline detailではなく当該callのdetailsとなることを固定する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: 現行HEADの一時実行ではtimeout後follow-upの `ok_details=''` を確認。実装は現状正しいがcommitted testがclearのregressionを検出しない。
- Notes: none

### REVIEW-20260711-005: PHPT 033の通常grpc-go fixtureはRST後の遅延frameを1本も発生させず、closed-stream処理によるreuse安全性を検証していない

- Severity: `Low`
- Status: `Open`
- Reviewer role: `adversarial test / compatibility reviewer`
- Finding: PHPT 033はserver delayを2秒、deadlineを300msにする一方、test全体は約0.95秒で終了する。さらにgrpc-go serverはclient RSTでcontextをcancelし、sleep後のSend/return responseをwireへ出さない。このためSPECと実装コメントがreuse安全性の根拠とする「RST後に到着したcancel済みstream宛frameをnghttp2がclosed-streamとして処理し、HPACK / connection flow-controlを同期する」経路は実際には一度も通らない。
- Evidence: `tests/phpt/033-deadline-rst-stream-connection-reuse.phpt:33-59`、`poc/test-server/main.go:90-120,356-375`。unary RST後に2.3秒待ってからfollow-upを実行したtraceでも、reset対象streamへの `wire.frame_in` はRST後0本 (`late_frames=[]`) だった。PHPTのRST traceはclient側送出のみを示す。
- Expected model: RSTとcrossした既送信frame、またはRSTを読む前にserverが送信予約したHEADERS / DATAが後から同じTCP connectionへ届いても、そのstreamのstale call stateへ触れず、HPACK / flow-controlを保ったまま後続streamが成功することをraw protocol fixtureで固定する。
- Why it matters: connection reuseのprotocol安全性を支える中心的な仮定だが、通常grpc-go fixtureは正しくcancelへ従うため敵対的なwire順序を生成できない。callback user-dataのunregister、closed-stream DATA、late HEADERS、WINDOW_UPDATEの回帰がPHPT 033では見えない。
- Recommended fix: raw h2c fixtureでrequest受信後にresponse HEADERS / DATAを送信予約し、client RSTとcrossするよう遅延送信した後、同じconnectionの次streamへ正常responseを返す。dynamic-tableを使うHEADERSとWINDOW_UPDATEを要するDATAを含め、RST対象streamのlate frameがtraceに存在すること、prefaceは1本、follow-upは `persistent_reused=true` / `STATUS_OK`であることをassertする。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: 通常fixtureを2.3秒待つ一時probeでlate frame 0本を確認。64KiB未満の「RST時点ですでに到着済み」backlogは別probeで安全にdrain/reuseできたが、RST後到着とは異なる。
- Notes: fixtureはRST後に新規送信を始めるprotocol違反を作る必要はなく、RSTをまだ読んでいないserver側writeとclient RSTをcrossさせれば実ネットワークで起こり得る順序を再現できる。

### REVIEW-20260711-006: PHPT 035はcancel時点のbacklog量を同期せず、preflight cap fallback assertionが非決定的

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `adversarial test / compatibility reviewer`
- Finding: PHPT 035は64KiB messageを1件yieldした直後に `cancel()` し、すぐfollow-upを開始する。serverへ200 messagesの送信を依頼していても、cancel時点でsocketに64KiB超がpendingである保証はない。backlogがcap未満ならpreflightはread boundaryへ到達して同connectionを再利用するため、testが期待する `persistent_reused=false` / preface 2本はschedulerとsocket timingに依存する。
- Evidence: `tests/phpt/035-preflight-drain-cap-fallback.phpt:33-41,72-77`。別reviewerが単発で `persistent_reused=true` failureを確認。こちらでもmodule rebuildなしの12回反復でPASS 11 / FAIL 1を再現し、FAILは `follow-up fell back to a fresh connection: expected false, got true`。
- Expected model: cap fallback testは、client cancelより前にserverがcap超過bytesを送信済みであることをprotocol fixtureのhandshakeで決定的に同期し、その事前条件をtrace/server signalでassertしてからfresh connectionを期待する。
- Why it matters: 本PRでbest-effort reuseの境界を固定する唯一の回帰testがflakyで、正常なreuseを誤ってfailure扱いする。CIで稀に落ちるだけでなく、fixture条件を満たさないPASS/FAILからpolicy保証を判断できない。
- Recommended fix: sleep延長だけに依存せず、raw h2c fixtureとout-of-band barrierを追加する。fixtureはfirst response messageを送った後、さらに `GRPC_LITE_PREFLIGHT_DRAIN_MAX_BYTES` を確実に超えるDATAを元connectionへwrite完了してからatomicな `backlog_ready` stateを立てる。clientは同じHTTP/2 connectionを読まず、別control port/connectionでそのstateをpollしてからcancelする（同connection上の同期frameはbacklogの後ろに並び、読むと事前条件を壊すため不可）。そのうえでpreflight traceに65536-byte full read / cap fallbackが存在することもassertする。fixture側でRST受信時の既送信byte数を記録・control endpointから照会できれば、事前条件をさらに直接固定できる。
- Fix summary: raw h2c data fixture `:50068` とout-of-band control `:50069` を追加。`arm` 後のfirst streamへ1 messageを返して保持し、`flood` でserver `SO_SNDBUF` を4KiBへ縮小して48KiBのDATAを書き、write完了後にだけ `ready` を返す。PHPTは同connectionを読まずcontrolを待ってからcancelする。client TCP window内で決定的にcap超過を作れるようtest processだけ `grpc_lite.preflight_drain_max_bytes=16384` とし、preflight readがcapへ達したこと、fresh connection、preface 2本をassertする。
- Fix commit: `b13362b`
- Verification (pre-fix): 次の反復commandで再現。

```bash
docker compose run --rm --no-deps dev bash -lc 'cd /workspace; pass=0; fail=0; for i in $(seq 1 12); do rm -f tests/phpt/035-preflight-drain-cap-fallback.{log,out,diff,exp,php,sh}; TEST_PHP_EXECUTABLE="$(command -v php)" php /usr/local/lib/php/build/run-tests.php -q -d extension=/workspace/modules/grpc.so /workspace/tests/phpt/035-preflight-drain-cap-fallback.phpt >/tmp/pr29-035-$i.log 2>&1; rc=$?; if [ $rc -eq 0 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo ITERATION:$i; sed -n "/FAILED TEST SUMMARY/,$ p" /tmp/pr29-035-$i.log; if [ -f tests/phpt/035-preflight-drain-cap-fallback.diff ]; then sed -n "1,120p" tests/phpt/035-preflight-drain-cap-fallback.diff; fi; fi; done; rm -f tests/phpt/035-preflight-drain-cap-fallback.{log,out,diff,exp,php,sh}; echo PASS:$pass FAIL:$fail; test $fail -eq 0'
```

結果: `PASS:11 FAIL:1`。iteration 4のdiffは `expected false, got true` (`persistent_reused=true`)。
- Verification: HEAD `be1b97e` の034 / 035 / 036同時反復で `PASS:30 FAIL:0`。直接probeでもcontrol=`armed` / `ready`、preflight 49,179 bytes、follow-up `STATUS_OK`, `persistent_reused=false`, preface 2本を確認。旧HEADの `PASS:11 FAIL:1` flakeは解消した。
- Notes: 本findingはテストの決定性に限定しFixed。新INIのstrict max semanticsはREVIEW-20260711-008として分離する。

### REVIEW-20260711-007: dead connectionを検知したsurvivorがtransport failureを保持せず `UNKNOWN` を返す

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `adversarial test / compatibility reviewer`
- Finding: 別ownerがshared connectionをdead化した後、server-streaming survivorの `next()` は `connection_io_allowed()` guardでI/O再駆動を止めるが、`state->completed = true` とするだけでcall側へtransport failureを記録しない。fixtureではstream Aがresponse headersとmessageを1件受信済みで、unary Bが同じconnectionの`ECONNRESET`を観測している。それでもAのcall stateは `http_status=200`, `grpc_status=-1`, `stream_closed=false` のままstatus resolverへ入り、最終fallbackの `STATUS_UNKNOWN(2)` を返す。gRPC Coreの公式status-code tableはclient側で「Some data transmitted ... before connection breaks」を `UNAVAILABLE` としており、既知のconnection failureをunknown errorへ落とす現状は `Grpc\` API互換性を満たさない。
- Evidence: `src/server_streaming_call.c:257-269,368-370`、`src/status_core.c:10-71`、`tests/phpt/034-dead-connection-terminal-for-owners.phpt:45-49`。直接probeの実値はunary B=`UNAVAILABLE(14)` / details=`recv failed: Connection reset by peer`、survivor A=`UNKNOWN(2)` / 同details。公式規定: [gRPC Core: Status codes and their use in gRPC](https://grpc.github.io/grpc/core/md_doc_statuscodes) のclient-generated status table。
- Expected model: serverからvalidなterminal `grpc-status` を受信する前にtransport connectionが破断し、当該callがdeadline / caller cancellationで終わったのでもない場合、そのconnectionを所有するin-flight callは `STATUS_UNAVAILABLE` で終了する。error detailsは当該connection failureを示し、別callのstale statusへ依存しない。
- Why it matters: callerは `UNAVAILABLE` を一時的なtransport failureとしてretry判断に使う一方、`UNKNOWN` は意味不明なstatus/error-space変換等を表す。dead後のhangを直してもterminal statusが誤れば、official `ext-grpc` のdrop-in replacementとしてretry・logging挙動が変わる。port 50066 fixtureとguardは本PRのdead-owner fixとして追加されたため、元issueのscope内でMediumとする。
- Recommended fix: `grpc_call` に明示的なconnection/transport failure state（例: `transport_failed` とcall-owned error details）を持たせ、guardがdeadを観測した時点でconnectionのerror detailをcallへcopyしてからcompleteする。status resolverではdeadline / caller cancellation / valid server `grpc-status` の既存優先順位を保ちつつ、terminal status未受信のtransport failureを `GRPC_STATUS_UNAVAILABLE` へ分類する。PHPT 034は単なる `code !== STATUS_OK` ではなくsurvivorの `STATUS_UNAVAILABLE` とtransport failure details（少なくとも非emptyかつconnection failure由来）をassertする。status taxonomyのC unitにも同stateを追加する。
- Fix summary: `grpc_call.connection_broken` を追加。dead-owner guardでconnectionのerror detail / errnoをcall-owned stateへsnapshotし、status resolverはdeadline / caller cancel / valid wire status / RSTの後、UNKNOWN fallbackの前で `UNAVAILABLE` を返す。PHPT 034をexact `UNAVAILABLE` + non-empty detailsへ強化し、C status unitへ優先順位5ケースを追加。
- Fix commit: `b13362b`
- Verification: HEAD `be1b97e` のPHPT 034 PASS、3本同時30回反復もPASS。port 50066直接probeはunary B / survivor Aとも `UNAVAILABLE(14)`、detailsとも `recv failed: Connection reset by peer`。traceはpreface 1本、server-streaming `:path` 1回、rpc.endも1回でtransparent retryなし。
- Notes: 本findingの「別ownerが先にconnectionをdead化し、次のpull先頭でguardが観測する」経路はFixed。同一pull中に当該call自身がEOF/errorを観測する非対称経路はREVIEW-20260711-010へ分離する。

### REVIEW-20260711-008: `preflight_drain_max_bytes` が1回のread sizeを制限せずconfigured maxを超えて消費する

- Severity: `Low`
- Status: `Open`
- Reviewer role: `adversarial test / compatibility reviewer`
- Finding: 新INI `grpc_lite.preflight_drain_max_bytes` はloop開始条件の `total_read < max_bytes` にしか使われず、各 `SSL_read()` / `recv()` は常に64KiB scratch全体を要求する。したがって16KiBを設定しても最初のreadで最大64KiBを消費し、名称が示すstrict maximumを最大約64KiB-1だけovershootできる。PHPT 035も `$preflightBytes >= 16384` としかassertしないため、この契約ずれを成功扱いする。
- Evidence: `src/transport.c:926-995`。HEAD `be1b97e` の直接traceはINI=`16384` に対し `wire.socket_preflight_read requested_len=65536, result_len=49179`。`tests/phpt/035-preflight-drain-cap-fallback.phpt:14,87-95` は16KiB設定と `>=` assertionだが、コメント `:60-62` は誤って「64KiB drain cap」と記述する。
- Expected model: `max_bytes` と呼ぶresource limitは1 iterationごとのread lengthも `min(buffer_len, max_bytes - total_read)` に制限し、累計が設定値を超えない。thresholdとしてovershootを許す設計なら名前・SPEC・phpinfoを `threshold` semanticsへ揃える。
- Why it matters: fallback自体は安全側に働き、production default 64KiBではscratchと一致するため既定挙動は変わらない。一方、operator-facingなSYSTEM INIが要求量を拘束しないため、調整時のresource予算と観測値が一致しない。bounded overshootでcorrectness failureではないためLowとする。
- Recommended fix: plaintext / TLSの双方で次readのrequested lengthをremaining budgetへclampし、PHPT 035はpreflight累計および各 `requested_len` がexact 16384以下であることをassertする。SPEC §4.2は「configured cap (default 64KiB) / 64 iterations」と記述し、PHPTコメントも16KiBへ直す。min clamp 4096の契約もINI testまたはdocsへ記録する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: defaultは `ini_get(...) === '65536'`、`PHP_INI_SYSTEM` によりprivate `grpc_lite.*` 設定として追加され、既存 `Grpc\` public API/default transport挙動は不変。custom 16384が実際のthreshold判定に使われる一方、traceで49,179-byte readへのovershootを確認。
- Notes: none

### REVIEW-20260711-009: PHPT 036はpending DATA provider / destructor / second ownerを作らず、元UAF lifecycleを実行しない

- Severity: `Low`
- Status: `Open`
- Reviewer role: `adversarial test / compatibility reviewer`
- Finding: PHPT 036はGOAWAY(MaxInt32)後の明示 `cancel()` がRST_STREAM(CANCEL)を出すことは固定するが、元Highのmemory-safety到達条件は通らない。requestはempty `BenchRequest` で、5-byte gRPC frameがEND_STREAM付きでGOAWAY前に完送されるためnghttp2にpending outbound DATA providerがない。stream/ownerも1本だけで、resource destructorによるcall/request free後に別admitted ownerがWINDOW_UPDATEを処理してsessionをsend駆動する段階もない。
- Evidence: `tests/phpt/036-draining-connection-cancel-sends-rst.phpt:28-44`、fixture `poc/test-server/main.go:591-656`。直接traceはserialized request payload=0、outbound stream DATA=`frame_payload_len=5`, flags=`END_STREAM`、stream ID 1のみ。その後GOAWAY→RSTのhappy pathだけを観測した。
- Expected model: UAF regression gateはsmall remote initial stream windowでAのlarge request DATA providerをpendingにし、Bもadmitした後にGOAWAYを受信、Aをdestructorで破棄し、peer WINDOW_UPDATE / B nextで同じdraining sessionを再駆動する。AへのRSTとBの正常完了をassertし、修正前実装がASan/UBSanでdangling `source.ptr` を検出する形にする。
- Why it matters: 現PHPTは三つのgateを `connection_io_allowed()` へ揃えた修正のうち明示cancelを守るが、destructorだけが再び `connection_usable()` へ戻る、またはpending provider detach順序が壊れる回帰を検出しない。ただし現production codeの3箇所のgate修正とRST flush orderingは静的にadequateなためLowとする。
- Recommended fix: test専用request paddingまたはlarge-request RPCとraw fixtureを追加し、small `SETTINGS_INITIAL_WINDOW_SIZE` + two admitted streams + GOAWAY + A resource destruction + WINDOW_UPDATE/B driveをPHPT 036（またはsanitizer専用test）で構成する。traceではAのrequest DATAがEND_STREAM前で止まったことをpreconditionとしてassertする。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: PHPT 036単体と30回反復はPASSしRST semanticsは安定。ただしtrace/stateの前提が上記UAF lifecycleと異なることを確認。
- Notes: PHPT 036の現タイトル「draining connection上のcancelがRSTを送る」に対してはtestは正しい。問題はdomain reviewのHigh UAFをこの1本で実機検証済みと扱うことに限定する。

### REVIEW-20260711-010: 同一pull中にcall自身がconnection breakを観測すると `connection_broken` が立たず `UNKNOWN` のまま

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `adversarial test / compatibility reviewer`
- Finding: `connection_broken` をsetするのはserver-streaming pull loop先頭で「別ownerがすでにdead化したconnection」を観測するguardだけである。entry時はconnectionがusableでも、そのpullの `connection_recv()` がEOF/transport errorを返す、またはtransport sendが失敗する分岐はconnectionをdead化してcompleteするだけでcall flagを立てない。response開始済み（`:status 200`）ならHTTP fallbackは使えず、wire grpc-statusも無いため最終 `UNKNOWN(2)` となる。field commentの「killed by its own I/O failure or by another owner」とも不一致。
- Evidence: `src/server_streaming_call.c:260-280` 対 `:288-330`、`src/status_core.c:63-76`、`src/grpc_exchange_state.h:47-51`。port 50069で `arm` 後、port 50068 streamから1 messageを受信し、同control connectionで再度 `arm`（target TCP connectionをclose）して直後の `next()` にdirect EOFを発生させた。実値は `valid=false`, `STATUS_UNKNOWN(2)`, details=`connection closed`。traceはpreface 1本、stream request 1 attempt、rpc.end status 2でtransparent retryなし。
- Expected model: validなterminal wire statusより前にclientが観測したconnection breakは、それを最初に観測したcall自身か別owner経由かに関係なくcall-owned transport failureとしてsnapshotし、deadline/cancel/wire status/RST等が無ければ `UNAVAILABLE` に分類する。response開始済みなのでtransparent retryは行わない。
- Why it matters: 同じTCP failureが「別callが先に観測したか」「このcallのrecvが先に観測したか」というscheduler順序だけでUNKNOWN / UNAVAILABLEに分裂する。official gRPC client taxonomyとretry/logging互換性を壊すためMediumとする。
- Recommended fix: transport connection failureをcallへ反映する共通helperを作り、unary / server-streamingのsocket/TLS send・recv EOF/error分岐で、より特異的なdeadline/cancel等を保ったまま `connection_broken` とdetails/errnoをsetする。direct nghttp2 fatalはNOMEM等の固有taxonomyを別途保ち、本flagへ無条件に畳み込まない。raw fixtureでresponse開始後のdirect EOF/resetをunary / server streaming双方に作り、exact UNAVAILABLE、transport details、attempt 1回をassertする。C unitはstatus priorityだけでなくtransition元のPHPTで補完する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: HEAD `be1b97e` のdirect EOF probeで再現。shared-owner :50066 probeはUNAVAILABLEへ直っており、本findingは同一pull内のself-observed breakに限定する。
- Notes: probe後はcontrol fixtureに残ったarmed stateをsacrificial h2 prefaceで消費し、後続PHPT 035へ影響しないことを確認。

## Review Result

- Blocker: none
- High: none
- Medium: 6 (`Fixed: 3 / Accepted: 1 / Open: 2`)
- Low: 4 (`Open: 4`)
- Design Decision: none
- Remaining gate: `Medium 2 / Low 4`
