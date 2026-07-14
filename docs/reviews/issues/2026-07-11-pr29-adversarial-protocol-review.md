# PR #29 deadline RST_STREAM / persistent reuse 敵対的protocol review 2026-07-11

## Scope

- PR #29: base `e49d4be`..HEAD `2af2d58`
- 第三パス対応commit: `d54a2ba` (`be1b97e..2af2d58`、実装差分は `d54a2ba`)
- 元issue: `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md`
- `src/transport.c`, `src/transport.h`, `src/grpc_exchange_state.h`
- `src/unary_call.c`, `src/server_streaming_call.c`
- `tests/phpt/033-deadline-rst-stream-connection-reuse.phpt`
- `tests/phpt/034-dead-connection-terminal-for-owners.phpt`
- `tests/phpt/035-preflight-drain-cap-fallback.phpt`
- `tests/phpt/036-draining-connection-cancel-sends-rst.phpt`
- `tests/phpt/037-draining-destructor-pending-request-data.phpt`
- `tests/phpt/038-fatal-rst-submit-marks-connection-dead.phpt`
- `tests/phpt/039-fatal-submit-request-marks-connection-dead.phpt`
- `poc/test-server/main.go` fixture `:50066`〜`:50070`
- `grpc.c`, `src/module.h`, `src/status_core.c`, `tests/unit/test_status_core.c`
- `docs/SPEC.md`, `docs/design/grpc-call-exchange-state.md`, `docs/verification/protocol-model-review-guide.md`

## Reviewer Role

- `HTTP/2 lifecycle / domain model adversary`

## Review Prompt Summary

- 既存のall-clear判定を前提にせず、deadlineをstream-localな`RST_STREAM(CANCEL)`へ変換してconnectionを温存する変更について、request HEADERS送信前を含むnghttp2 stream lifecycle、session-wide pending frame flush、multiplex中のcall deadline、late response backlog、preflight drain、persistent reuseの安全性とテストの実効性を、元issueのスコープ内で敵対的に確認した。
- HEAD `6795a5a` 再レビューでは、SPECのbest-effort化、dead / draining predicate分離、PHPT 033強化、PHPT 034/035とfixture `:50066`を確認した。productionのbest-effort fallback自体は安全だが、PHPT 035のbacklog条件が非決定的であること、およびGOAWAY draining中のadmit済みstreamでcancel predicateが誤っていることを追加検出した。
- HEAD `be1b97e` 再レビューでは、dead後のlocal-only cleanup、draining上のstream close、`connection_broken` taxonomy、preflight drain capのsystem INI化、fixture `:50067`〜`:50069`のbarrierを確認した。前回Medium 2件はadequateに修正された。capを調整してもEAGAIN / WANT_READ境界を確認した場合だけconnectionを再採用し、cap / iteration到達時は必ずdrainingへ落とすためsafe adoptionは維持される。
- HEAD `2af2d58` 第四パスでは、same-pullのsocket/TLS send・recv failureへの`grpc_call_note_connection_broken()`適用、socket/TLS preflight read長のstrict cap、callback内RST fatalのunwind、original UAF shapeのfixtureを確認した。前回Medium 1 / Low 2のproduction fixは意図した経路では有効。一方、server streamingのterminal framing判定がconnection breakを`INTERNAL`へ上書きするcall-kind不整合、unary callback-policy fatalがstatusではなくexceptionになる優先順位漏れ、test fault seamのproduction混入を追加検出した。

## Issues

### REVIEW-20260711-001: request HEADERS未送信のidle stream IDへRST_STREAMを送れてconnection errorを作る

- Severity: `High`
- Status: `Rejected`
- Reviewer role: `HTTP/2 lifecycle / domain model adversary`
- Finding: 当初、peerの`SETTINGS_MAX_CONCURRENT_STREAMS`上限によりrequest HEADERSがoutbound queueで待機している状態で`nghttp2_submit_rst_stream()`を呼ぶと、pending HEADERSがcancelされてidle stream IDのRSTだけがwireへ出ると推測した。しかしこれは誤りだった。nghttp2は未送信requestを内部でcancelし、peerへRST_STREAMを送らない。
- Evidence: `SETTINGS_MAX_CONCURRENT_STREAMS=1`を送るraw HTTP/2 fixtureで、stream 1をopenのまま保持し、stream 3のrequest HEADERSが未送信の状態でdeadline helperを実行した。client traceにもserver観測にもstream 3のHEADERS / RST_STREAMはなく、serverは`PROTOCOL_ERROR`を観測せず、stream 1とconnectionは維持された。独立したtest reviewerも同じ挙動を確認した。`src/unary_call.c:155-173,192-198`、`src/transport.c:319-337`のコード経路自体は通るが、nghttp2のpending-frame cancel処理がwire-invalidなframeを抑止する。
- Expected model: request HEADERS未送信のpending requestは、nghttp2がoutbound queue上で内部cancelし、peerから見えるHTTP/2 streamもRST_STREAMも生成しない。request HEADERS送信済みのstreamだけがwire上のRST_STREAM(CANCEL)対象になる。現HEADはこのモデルに従う。
- Why it matters: 当初想定したconnection-level `PROTOCOL_ERROR`と無関係なactive streamの巻き込みは発生しないため、High findingの根拠がない。
- Recommended fix: 修正不要。MAX_CONCURRENT_STREAMS到達中のdeadline cancelが「pending HEADERSなし / idle RSTなし / connection継続」であることをraw fixture regressionとして残すと、このnghttp2依存の境界を固定できる。
- Fix summary: `Rejected: nghttp2が未送信requestを内部cancelし、wire RSTを生成しないことを実測`
- Fix commit: `N/A`
- Verification: raw fixtureとclient wire traceで、stream 3のHEADERS / RST_STREAMおよびconnection `PROTOCOL_ERROR`がいずれも発生しないことを確認。
- Notes: pending request cancelで`on_stream_close_callback()`が必ず呼ばれるとは扱わない。request HEADERS未送信ならnghttp2 stream objectが作られていない場合があり、wire stream close callbackはcleanupの必須条件ではない。unaryはdeadline分岐後の`clear_connection_call_owner()`、server streamingはcompleted後のowner clearでapplication側登録を外せるため、このRejected findingを復活させる根拠にはならない。

### REVIEW-20260711-002: 64KiBのclosed-stream response backlogだけでpreflightがconnectionを捨て、温存目標を満たさない

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `HTTP/2 lifecycle / domain model adversary`
- Finding: local RST後に到着済みbytesがあると、次callの`preflight_persistent_connection()`は最大64KiBをdrainする。しかし1回の`recv()`がscratch全量の65536 bytesを返した時点で`total_read < GRPC_LITE_PREFLIGHT_DRAIN_MAX_BYTES`がfalseになり、socketがちょうど空になった場合でもEAGAIN境界を確認する次のprobeを行わない。`reached_read_boundary`はfalseのままなのでconnectionをdrainingにし、次callは新規TCP connectionへ移る。さらにbacklogが本当に64KiB超なら同じfallbackとなる。nghttp2がreset済みstreamのlate frameを安全にminimal-processできることを根拠にpost-RST drainを省いた一方、既存preflight上限がその正当なlate frameをconnection破棄理由へ変えている。
- Evidence: `src/transport.c:902-988,1017-1050`、`docs/SPEC.md:92`、元issue Decision Log。手動reproとして`BenchServerStream(message_count=1000, payload_bytes=65536, timeout=300000us)`を開始し、最初のmessage後に500ms停止してdeadlineを越え、直後に`SayHello`を実行した。結果はstreaming `DEADLINE_EXCEEDED`、outbound RST_STREAM(CANCEL) 1件、`wire.socket_preflight_read result_len=65536` 1件、connection preface 2件、後続`SayHello`はSTATUS_OKだが`persistent_reused=false`だった。
- Expected model: reset済みstreamのlate HEADERS/DATAをnghttp2が安全に処理できたなら、そのbytesが64KiBに達したこと自体をconnection failure/drainingへ分類しない。preflightのwork boundとconnection reuse可否は別概念として扱い、少なくとも「full-buffer read後にread boundaryを未確認」と「protocol/sessionが壊れた」を同一視しない。
- Why it matters: slow consumer、large response、deadline直前にserver送信が進んだ通常ケースで、本issueが提供するはずのTCP/TLS handshake回避が確実に失われる。status correctnessは保たれるが、元issueの主要なproduction効果が小さい遅延応答fixtureにしか成立しない。
- Recommended fix: full-buffer read後にEAGAINを確認できるprobe余地を設けるだけでなく、64KiB超のclosed-stream backlogをboundedに処理しながらconnectionを再利用する方針を決める。例えばpreflightの1回当たりwork上限に達しただけならsessionをdrainingにせず、残りを次callのrecv loopへ引き継ぐためのstateを持つ。あわせて、RSTを受けても既送信DATAを同connectionへ流し続け、その後のRPCにも応答するraw fixtureで`persistent_reused=true`を固定する。単純なcap増加だけでは境界値とlarge backlogの問題を先送りする。
- Fix summary: bounded adoptionは本PRのスコープ外として採用せず、SPEC §4.2を「reuseはbest-effort」に修正した。preflightの64KiB / 64 iterations cap超過はprotocol failureではなく、安全側のdraining化と新規connectionへのfallbackであり、後続RPCの成功を保証する契約へ弱めた。このproduction判断は妥当。cap fallbackを固定する目的でPHPT 035も追加されたが、到着済みbacklogの作り方が非決定的であるため、テスト側の残件はREVIEW-20260711-006へ分離する。
- Fix commit: `0480479`
- Verification: `docs/SPEC.md:92`、`src/transport.c:914-1000`、PHPT 035を再レビュー。HEAD `6795a5a`のDocker targeted runでは、同じテストが一度は`persistent_reused=true`でFAILし、直後の単独再実行はPASSした。これはproductionのbest-effort契約違反ではなく「capを超えるbytesがcancel前に到着済みか」がrunごとに異なるためである。
- Notes: baseではdeadline時点でconnectionを破棄していたため、preflight上限自体は既存でも、本PRのconnection温存仕様を不完全にする形で初めて顕在化した。現在のSPECはその限界を正しく表す。

### REVIEW-20260711-003: PHPTの「in-flight survivor」はRST送信前に既にtrailersまで受信してcloseしている

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 lifecycle / domain model adversary`
- Finding: PHPT 033の並行ケースはsurvivorを3 messages / 100ms間隔、競合unaryを300ms deadlineで実行する。survivorの1件目をyieldした直後にunaryへ入るため、unaryのrecv loopが100ms後と200ms後の残り2 messagesおよびtrailersをdispatchし、survivorはunaryのRSTより前にnghttp2上でclose/unregisterされる。したがってassertion名とコメントに反して、RST flush時の`active_stream_count > 1`、別streamのpending control/DATA、RST後も生存するstreamを一つも検証していない。
- Evidence: `tests/phpt/033-deadline-rst-stream-connection-reuse.phpt:61-80`、`poc/test-server/main.go:356-375`。HEADでこのケースを単独traceしたところ、survivor stream 1の3件目DATAとEND_STREAM trailers(`HEADERS flags=5`)はmonotonic_us `1883805618651` / `1883805618662`、timeout unary stream 3のoutbound RSTは`1883805715683`で、survivor closeが約97ms先行していた。
- Expected model: multiplex safetyを固定するテストでは、timeout streamへRSTを送る瞬間に別streamがnghttp2上でactiveであり、RST後にもそのstreamのframeを受信してSTATUS_OKまで完了することをwire/state evidenceで示す。
- Why it matters: `setup_deadline_abs_us`修正とsession-wide cancel flushが無関係なstreamを壊さないという回帰gateが、実際には直列完了ケースしか見ていない。将来同じscope leakが戻ってもPHPTは通る。
- Recommended fix: survivorのmessage間隔をunary deadlineより十分長くし、RSTより後にsurvivor DATA/trailersが到着するfixtureへ変更する。traceでsurvivorのstream IDがRST時点でactiveだったこと、または少なくともsurvivorのterminal HEADERSがRSTより後であることもassertする。
- Fix summary: survivorのmessage間隔を100msから700msへ変更し、traceでsurvivor stream idを特定したうえで、当該stream宛RSTがないこと、terminal HEADERSがconcurrent unaryのRSTより後であること、3 messages + STATUS_OKをassertした。
- Fix commit: `0480479`
- Verification: HEAD `6795a5a`のDocker targeted runでPHPT 033 PASS。`tests/phpt/033-deadline-rst-stream-connection-reuse.phpt:114-147`のwire順序assertionも確認し、元のfalse-positive余地は解消した。
- Notes: response countと最終STATUS_OKだけでは、frameがunary待機中にqueue済みだったケースを区別できない。

### REVIEW-20260711-004: 新設`locally_cancelled` / `connection_broken` lifecycle stateが責務mapから欠落している

- Severity: `Low`
- Status: `Open`
- Reviewer role: `HTTP/2 lifecycle / domain model adversary`
- Finding: PRはlocal RSTとinbound RSTを分離するため`grpc_call.locally_cancelled`を追加し、truncated-response taxonomyの判定に使う。さらに`b13362b`はshared connection deathをcallへsnapshotする`connection_broken`を追加し、status priorityへ使う。しかし`grpc_call`の正規responsibility mapはstream lifecycle / reset stateを`stream_closed`, `stream_error_code`, `stream_reset_seen`, `stream_refused_seen`だけ、deadline / I/O failureを従来fieldだけとしており、両stateのowner、set point、status/parser上の意味を記録していない。
- Evidence: `src/grpc_exchange_state.h:38-56`、`src/transport.c:325-359`、`src/server_streaming_call.c:260-280,372-378`、`src/status_core.c:63-67`に対して、`docs/design/grpc-call-exchange-state.md:7-25`のresponsibility map。
- Expected model: exchange-state文書は現行`grpc_call`のfield ownership mapであり、local cancel initiationとpeer reset observationという別概念を明示する。現実装では`locally_cancelled`はRST submit前にsetされ、submit / flush失敗時もcall-localなcancel intentとして残り、truncated response判定だけを除外する。
- Why it matters: 今回のfixは「local CANCELをmalformed server responseへ混ぜない」「wire status / deadline / cancel / resetをconnection breakより優先する」というtaxonomy分離に依存する。正規mapから欠けると、将来`stream_reset_seen`への誤統合やpriority変更を誘発する。
- Recommended fix: responsibility mapへ両fieldを追加する。`locally_cancelled`は「local cancelを開始した事実(RST送出成功の証明ではない)」「inbound RST観測ではない」「truncated response判定から除外する」、`connection_broken`は「shared connectionのdeadをcallへsnapshot」「terminal wire status未受信時のUNAVAILABLE」「response-started / transparent retryとは別概念」とset/clear lifetimeを記載する。
- Fix summary: `pending`。HEAD `2af2d58`でも`docs/design/grpc-call-exchange-state.md`のstream lifecycle rowは`stream_closed`, `stream_error_code`, `stream_reset_seen`, `stream_refused_seen`のままで、両fieldは未記載。加えてSPEC §4.2本文はcapを固定`64KiB / 64 iterations`と記述する一方、実装はsystem INIでbyte capを変更可能(min 4KiB、default 64KiB)であり、変更履歴だけがINIを説明している。
- Fix commit: `pending`
- Verification: HEAD `2af2d58`の`docs/design/grpc-call-exchange-state.md` responsibility map、`docs/SPEC.md` §4.2、`src/grpc_exchange_state.h`を突合。
- Notes: design docには現在のmodelを残すというrepository方針に基づく指摘であり、過渡的なreview履歴の追記要求ではない。

### REVIEW-20260711-005: 50ms graceがunrelated DATAのsource call deadlineより優先される設計を明示的に決めていない

- Severity: `Design Decision`
- Status: `Accepted`
- Reviewer role: `HTTP/2 lifecycle / domain model adversary`
- Finding: `cancel_grpc_call_stream()`は`send_pending_h2_frames_with_deadline(connection, NULL, now+50ms)`でconnection-wideな`nghttp2_session_send()`を駆動する。`h2_send_data_callback()`はDATA sourceの`grpc_call *call`を受け取っているにもかかわらず、その`call->deadline_abs_us`ではなく`connection->current_write_deadline_abs_us`だけをwrite deadlineにする。そのためcancel flush時に別streamのsendable DATAがあれば、source call自身のabsolute deadlineが既に切れていても50ms graceで送信を進め得る。issue Decision Logは「pending frame一式を巻き込む量」と「50ms超過ならconnectionを捨てる」ことは受容しているが、別callのsemantic deadlineを上書きすることまでは判断していない。
- Evidence: `src/transport.c:306-345,1818-1867,1924-1969`、元issue Decision Log `:65-67`。同じsession-wide deadline借用は既存の通常`send_pending_h2_frames(connection, call)`にも存在し、productionの同期loopではsendable DATAを各sendで尽くすため、cancel helperだけで新しく再現しやすくなった実害までは確認していない。
- Expected model: connection control frameのwrite graceと、各gRPC callのrequest DATA deadlineは別scopeである。session-wide flushを採用するなら、どのdeadlineをどのframeへ適用するか、inactive callの期限切れをいつ観測するかを明示的に決める。
- Why it matters: multiplexを広げた際、あるcallのdeadline cancelが別callの期限切れrequestをserverへ送る、または50ms write failureで別callまでconnection failureへ巻き込む挙動が暗黙の実装詳細になる。今回のconnection温存判断の安全境界でもある。
- Recommended fix: (a) cancel flush中は対象RSTとconnection control frameだけを進め、unrelated DATAをdeferする、(b) unrelated DATAにはsource call deadlineとcancel graceの短い方を適用する、(c) session-wide graceで全sendable DATAを進める現設計を制約込みで受容する、のいずれかを明記する。(b)でcallback failureを返すだけではconnectionを殺すため、nghttp2のdefer/resume lifecycleまで含めて設計する。
- Fix summary: 元issue Decision Logが、50ms graceを「RSTだけでなく、その時点のsession pending frame一式（並行streamのWINDOW_UPDATE / SETTINGS ack / 送信可能DATAを含む）のflush上限」と明記し、超過時にconnection全体をdeadへ縮退する制約を明示的に受容している。現在の同期poll modelでは、別callのdeadlineはそのcallが次にdriveされた時点で観測するという既存制約もNon-Goalの範囲として扱う。
- Fix commit: `caeac40`（Decision Log追記。現HEADでも維持）
- Verification: 元issue Decision Logと`cancel_grpc_call_stream()` / `send_pending_h2_frames_with_deadline()`を突合。
- Notes: 元issueの「deadline検出精度は変更しない」というNon-Goalはpoll精度の話であり、別callのrequest DATAへどのwrite deadlineを適用するかというscope判断を自動的には解決しない。

### REVIEW-20260711-006: PHPT 035はcancel前に64KiB超のbacklog到着を保証せず、同じHEADでreuse / fallbackが反転する

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `HTTP/2 lifecycle / domain model adversary`
- Finding: PHPT 035は`message_count=200`, `payload_bytes=65536`のserver streamingを開始し、最初のmessageを1件受け取った直後に`cancel()`する。しかし「serverが大量にsendしようとした」ことと「client socketへpreflight cap超のbytesが既に到着した」ことは同じではない。clientが1件目を返した直後にRSTがserverへ届けば、後続DATAは64KiB未満しか到着せず、preflightは正常にread boundaryへ達して同じconnectionをreuseする。この正常なbest-effort挙動に対して、テストは`persistent_reused=false`とpreface 2本を必須にするためfalse failureになる。
- Evidence: `tests/phpt/035-preflight-drain-cap-fallback.phpt:31-40,73-78`。HEAD `6795a5a`で `docker compose run --rm dev bash -lc 'cd /workspace && TEST_PHP_EXECUTABLE="$(command -v php)" php /usr/local/lib/php/build/run-tests.php -q -d extension=/workspace/modules/grpc.so /workspace/tests/phpt/033-deadline-rst-stream-connection-reuse.phpt /workspace/tests/phpt/034-dead-connection-terminal-for-owners.phpt /workspace/tests/phpt/035-preflight-drain-cap-fallback.phpt'` を実行すると、033/034はPASS、035は `follow-up fell back to a fresh connection: expected false, got true` でFAILした。直後に035だけを同条件で再実行するとPASSし、独立test reviewerのfresh targeted runもPASSした。
- Expected model: cap fallbackのregressionは「cancel時点でcap超のbytesがclient側に到着済み」というpreconditionをfixture / synchronizationで決定的に作ってから、draining + new connectionをassertする。preconditionが成立しなかったrunでreuseできるのは、修正後SPECのbest-effort契約どおりである。
- Why it matters: CIがscheduler / socket timingでflakyになり、失敗時にproduction regressionとfixture raceを区別できない。同時に、PASSしても実際に64KiB cap pathを通ったことをtraceで直接確認していないため、fallback regression gateとして弱い。
- Recommended fix: raw HTTP/2 fixtureでfirst message後もRSTを受けるまでにcap超のDATAを確実にclient socketへqueueする同期点を設けるか、少なくともclient側で明示的なwaitを置いてpreflight traceの`wire.socket_preflight_read`累計がcapへ達したことをassertする。より堅牢にはfixtureから「backlog送出完了」を観測できるhandshakeを用意し、その後にcancelする。
- Fix summary: fixture `:50068` / control `:50069`を追加した。controlの`arm`で次connectionを対象化し、first message後の`flood`でserver `SO_SNDBUF`を4KiBへ縮小して48KiB DATAをwriteする。write完了後の`ready`をout-of-band barrierとし、PHP側がH2 socketを読まない間にclient kernelへbacklogを置く。production defaultは64KiBのまま `grpc_lite.preflight_drain_max_bytes`を`PHP_INI_SYSTEM`で追加し、PHPT 035だけ16KiBへ下げ、preflight read累計がcap以上・fresh connection・follow-up OKをassertする。
- Fix commit: `b13362b`
- Verification: HEAD `be1b97e`でPHPT 035 PASS。PHPT 035/036を追加5回反復して全PASS。fixtureのcontrol barrier、`wire.socket_preflight_read`累計assert、unarmed second connectionのnormal OK経路をcode reviewした。元issue記録のFAILベース23回連続PASSとも整合する。
- Notes: REVIEW-20260711-002のbest-effort化は維持される。cap INIの厳密なbyte-bound semanticsはREVIEW-20260711-009へ分離する。

### REVIEW-20260711-007: GOAWAY draining中のadmit済みstreamがdeadline / user cancelされてもRST_STREAMを送れない

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `HTTP/2 lifecycle / domain model adversary`
- Finding: HEADはdeadとdrainingを分離し、`connection_io_allowed()`でGOAWAYにadmit済みのstreamがI/Oを継続できるようにした。一方、`cancel_grpc_call_stream()`は依然として`connection_usable()`をgateに使う。`connection_usable()`はdrainingをfalseにするため、`stream_id <= GOAWAY.last_stream_id`のactive streamが後からdeadlineに達しても、helperは`locally_cancelled`のset、`nghttp2_submit_rst_stream()`、flushを全てskipする。server streamingの明示cancel / destructorにも同じ`connection_usable()` gateがあり、helperだけ直してもuser cancel経路は残る。
- Evidence: `src/transport.c:320-338,358-369`、`src/server_streaming_call.c:260-273,297-303,410-424`、`src/transport.c:855-869`。GOAWAY受信は`on_frame_recv_callback()`でconnectionをdrainingにする一方、last stream id以下はactive登録を維持する(`src/transport.c:2148-2164`)。そのstreamのread poll timeoutは`cancel_grpc_call_stream()`へ到達するが、line 329の`!connection_usable(connection)`でreturnする。
- Expected model: `connection_usable`は「新規streamをadopt / reuseできるか」、`connection_io_allowed`は「既にadmit済みのstreamがI/Oできるか」を表す。既存streamのRST_STREAMは後者に属し、deadでは禁止、drainingでは許可されるべきである。
- Why it matters: userland status自体はDEADLINE_EXCEEDED / CANCELLEDへ進めるが、wire上ではcancelされない。別のadmit済みownerがconnectionを保持する間、serverは期限切れRPCの処理とDATA送信を続け、shared connection flow-controlとserver resourceを消費する。これは元issueが解消しようとした「timeout後もserver処理が継続する」問題をGOAWAY + multiplex時に残す。
- Recommended fix: `cancel_grpc_call_stream()`のeligibilityを`connection_io_allowed()` + stream id / stream closed checkへ変更する。`server_streaming_call_cancel_resource()`とresource destructorのouter gateも同じpredicateへ揃える。raw fixtureで2 streamをadmitするlast_stream_idのGOAWAYを送り、一方をdeadline / user cancelしてoutbound RST_STREAM(CANCEL)を観測し、もう一方が完走することを固定する。
- Fix summary: `cancel_grpc_call_stream()`、server streaming明示cancel、resource destructorのclose eligibilityを`connection_io_allowed()`へ統一した。draining上でもRST submit + flushを行い、成功時はnghttp2がstream/data providerをcloseしてからcall stateを解放する。失敗時はdeadとなって以後sessionを駆動しない。fixture `:50067`はresponse message後にGOAWAY(MaxInt32)を送ってstreamをadmit済みのまま保持し、PHPT 036がexplicit cancelのRST_STREAM(CANCEL)を確認する。
- Fix commit: `b13362b`
- Verification: `src/transport.c:325-381`, `src/server_streaming_call.c:422-436`をreview。HEAD `be1b97e`でPHPT 036 PASS、PHPT 035/036の追加5回反復も全PASS。dead cleanupは`unregister_grpc_call_stream()`がsession user data操作をskipしlocal bookkeepingだけを行い、最後のowner後の`nghttp2_session_del()`まで再駆動されないことも確認した。
- Notes: production fixはadequate。PHPT 036が元のpending DATA UAF sequenceを直接再現しないtest coverage gapはREVIEW-20260711-010へ分離する。

### REVIEW-20260711-008: PHPT 034 / 036のevent ordering oracleがmicrosecond同値を正しく扱えない

- Severity: `Low`
- Status: `Open`
- Reviewer role: `HTTP/2 lifecycle / domain model adversary`
- Finding: PHPT 034はunary Bの`rpc.end.monotonic_us`より大きいtimestampのI/O eventだけを禁止するため、直後のbad I/Oが同じmicrosecondなら見逃す。PHPT 036 / 新規037もGOAWAYとRSTの順序を`RST monotonic_us > GOAWAY monotonic_us`で判定するため、正しい直後のRSTが同値なら逆にfalse failureとなる。trace clockはnanosecondsを1000で割ったmicrosecond精度であり、event順序の全順序ではない。
- Evidence: `tests/phpt/034-dead-connection-terminal-for-owners.phpt`、`tests/phpt/036-draining-connection-cancel-sends-rst.phpt`、`tests/phpt/037-draining-destructor-pending-request-data.phpt:73-86`、`src/transport.c`の`monotonic_us()`。JSONL append順序は保持されるためrecord indexで判定できる。
- Expected model: no-I/O-after-terminalの順序不変条件は、解像度の粗い時刻比較ではなくtrace recordの全順序で検証する。
- Why it matters: PHPT 034はdead session再駆動を防ぐ新guardの主回帰テストであり、最も問題となる「直後のnext()」だけが同一timestampでfalse negativeになり得る。
- Recommended fix: PHPT 034はunary `rpc.end` indexより後のrecordだけを走査し、PHPT 036はGOAWAY record indexより後に対象RSTがあることをassertする。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: test oracle / trace timestamp実装のcode review。HEAD targeted run自体はPASS。
- Notes: fixture `:50066` / `:50067`のprotocol順序自体は目的に合っている。本指摘はpostconditionの時刻比較に限る。

### REVIEW-20260711-009: `preflight_drain_max_bytes`は単発readを制限せず、設定値を最大64KiB超過する

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 lifecycle / domain model adversary`
- Finding: 新INIは`max_bytes`と命名され、PHPT 035は16KiB capとして扱うが、drain loopはloop conditionにしか値を使わず、各`SSL_read` / `recv`には常に64KiB scratch全量を渡す。したがって16KiB設定でも最初のreadで48KiB、最小clamp 4KiBでも最大64KiBを読み得る。これはstrictなbyte上限ではなく「次iterationへ進むかを決めるthreshold」である。
- Evidence: `grpc.c:21`、`src/transport.c:926-950,954-958,976-994`。PHPT 035自身も`preflightBytes >= 16384`としかassertせず、設定値超過を許容している。
- Expected model: `*_max_bytes`として公開するなら、1回のread lengthも`min(buffer_len, max_bytes - total_read)`にして設定値以内へ収める。意図がthresholdなら名称・SPEC・テストコメントをその意味へ揃える。
- Why it matters: protocol safetyは壊れない(cap到達後はdrainingで再採用しない)が、system operatorがpreflightの1回当たりwork / bytesを制限するknobとして解釈すると最大16倍(4KiB設定時)乖離する。
- Recommended fix: read lengthをremaining budgetでclampし、socket/TLS traceのrequested lengthも実budgetを反映する。合わせてPHPT 035の「64KiB cap」コメントをtest実値16KiBに修正する。
- Fix summary: 各iterationのread長を`min(recv_scratch_len, max_bytes - total_read)`へclampし、socketの`recv()`とTLSの`SSL_read()`、traceの`requested_len`を同じbudgetへ揃えた。PHPT 035は累計readがcap以上かつcap以下、すなわちexact 16KiBであることを固定した。
- Fix commit: `d54a2ba`
- Verification: `src/transport.c:1022-1073`をsocket/TLS両branchでreview。dev-sanitizer PHPでPHPT 035を単独実行し1/1 PASS。
- Notes: upper bound未設定でもiteration cap 64が残るため1 adoptionのread workは約4MiBまでに収まり、unbounded readにはならない。

### REVIEW-20260711-010: PHPT 036はexplicit cancelだけを通し、修正根拠のpending DATA provider UAFを再現しない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2 lifecycle / domain model adversary`
- Finding: PHPT 036はrequest DATA送信完了後の単一streamでexplicit `cancel()`を呼び、client traceにRSTがserializeされたことを確認する。これは`cancel_resource`とhelperのdraining predicateは通すが、resource destructorのpredicate、flow-controlでnghttp2に残った`data_provider.source.ptr`、別admit済みownerによる後続`session_send`を通さない。したがって元Highのmemory-safety sequenceを構成するdestructor側だけがregressしてもPHPT 036はPASSする。
- Evidence: fixture `:50067`(`poc/test-server/main.go:591-654`)は1 requestをEND_STREAMまで受けてからresponse/GOAWAYを送り、PHPT 036は`$call->cancel()`後もobjectを保持して`getStatus()`する(`tests/phpt/036-draining-connection-cancel-sends-rst.phpt:35-45`)。fixtureはRST受信を記録・ackせず、assertはactual socket flush前にも生成されるclient `wire.frame_out`だけである。
- Expected model: UAF regression gateは、peer SETTINGSでrequest DATAをflow-control pendingにし、GOAWAYで2 streamをadmitし、一方のresourceを明示cancelなしで破棄した後、out-of-band barrierでWINDOW_UPDATEを送り、survivorのnextがsessionを再駆動するsequenceをASAN下で通す。少なくともfixture側でRST受信をcontrol channelからassertし、survivor完走も固定する。
- Why it matters: production fixはstatic lifecycle review上adequateだが、最も深刻だったownership bugの再現条件をtestが所有しておらず、3箇所のgateが将来個別にずれても検出できない。
- Recommended fix: fixture `:50067`を複数stream + control channel対応へ拡張するか専用fixtureを追加し、flow-controlled provider / destructor / survivor driveを決定的に固定する。併せて`h2_send_data_callback`直前のlifetime commentを、drainingはsession drive可能であり「call解放前にstream close(RST)またはconnection deadが必須」とする現modelへ直す。
- Fix summary: fixture `:50070`がINITIAL_WINDOW_SIZE=1024でlarge request AのDATA providerをdeferし、BへGOAWAY(MaxInt32)+messageを返す。PHPT 037はdraining観測後にAをdestructor-onlyで解放し、遅延WINDOW_UPDATE(A)を跨いでBをdrive / STATUS_OK完走させる。DATA provider lifetime commentも「stream close(RST)またはdead後no drive」の二条件へ修正した。
- Fix commit: `d54a2ba`
- Verification: fixture `handleSmallWindowGoAwayDrainingH2C()`、PHPT 037、`h2_send_data_callback()`直前commentをcode review。production ownership sequenceは元のpending provider / destructor / second owner driveを満たす。
- Notes: PHPT 036はexplicit cancelのwire serialization gateとして引き続き有効で、PHPT 037が元のownership/UAF shapeを補完する。旧lifetime commentのdraining誤記も修正済み。

### REVIEW-20260711-011: 同じpull内でconnectionがdeadになると`connection_broken`を立てず、response開始済みstreamがUNKNOWNになる

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `HTTP/2 lifecycle / domain model adversary`
- Finding: `b13362b`はpull開始時にconnectionが既にdeadならloop先頭guardで`call->connection_broken=true`とtransport detailのsnapshotを行う。しかしconnectionがentry時点ではusableで、そのpull内のsend / recv / nghttp2 processingでdeadへ遷移した場合、各error branchは`mark_connection_dead()`と`completed=true`だけでstatus resolverへ進む。response headers / messageを既に受けたcallは`http_status=200`, `grpc_status=-1`, `stream_closed=false`, `connection_broken=false`となり、UNKNOWN fallbackへ落ちる。
- Evidence: `src/server_streaming_call.c:257-334`。markerを立てるのはline 260のpre-loop guardだけで、(1) initial `send_pending_h2_frames` failure(line 288-297)、(2) non-timeout `connection_recv` EOF/error(line 302-316)、(3) `nghttp2_session_mem_recv` fatal(line 318-324)、(4) post-recv `send_pending_h2_frames` failure(line 326-331)はいずれもmarker/snapshotを欠く。unaryにもinitial send、non-timeout recv、mem_recv、post-recv sendの同型branchがある(`src/unary_call.c:173-219`)が、mem_recv fatalはstatusではなくexceptionへ進む。fixture `:50068/:50069`で`arm`→first message受信→再度`arm`(server側existing TCP close)→同じgeneratorの`next()`を行うと、HEAD `be1b97e`は`code=2`, `details=connection closed`を返した。
- Expected model: SPEC §4.2と`grpc_exchange_state.h`の新commentどおり、valid terminal `grpc-status`、deadline、caller cancel、stream resetのいずれでも決まっていないcallがclient-observed connection breakで終わる場合は、breakを観測したpullが同じか次かに依存せずUNAVAILABLEとなる。response開始済みなのでtransport-level transparent retryは行わない。
- Why it matters: connection breakをUNKNOWNへ分類するとgax等のUNAVAILABLE retry policyから外れ、同じwire failureがPHPのpull境界だけでstatus 14/2へ揺れる。今回追加した`connection_broken` modelとSPECを通常のdirect EOF pathが満たさないためMediumとする。
- Recommended fix: connection breakをcallへsnapshotするsmall helperを作り、server streamingの上記4 branchで`mark_connection_dead()`直後かstatus resolution前に呼ぶ。unaryのresult-returning send / non-timeout recv / post-recv send failureにも同じmarkerを適用し、response開始後EOFのtaxonomyをcall kind間で揃える。deadline/caller cancel/wire status/resetの既存priorityと、refused保証がないconnection breakをtransparent retryしないattempt outcomeは変更しない。
- Fix summary: `grpc_call_note_connection_broken()`を追加し、unary / server streamingのinitial/post-recv send failure、non-timeout recv EOF/error、およびpull開始時dead guardへ適用した。deadlineはhelper no-op、wire grpc-status / cancel / reset / validation flagは既存status priorityで優先し、`connection_broken`自体はREFUSED predicateを立てないためtransparent retry対象にならない。
- Fix commit: `d54a2ba`
- Verification: affected branchesをcode review。dev-sanitizer focused probeでfixture `:50057`のunary direct connection breakはexact `UNAVAILABLE(14)`, details `recv failed: Connection reset by peer`を確認。partial-message server streamingの別priority defectはREVIEW-20260712-012へ分離。
- Notes: top-of-loop guardによる「別ownerが前pullとの間にdead化」は正しくUNAVAILABLEとなる。本指摘はcurrent ownerが同じpullでconnection breakを初めて観測するbranchに限定する。

### REVIEW-20260712-012: server streamingはmessage途中のconnection breakを`INTERNAL`へ上書きする

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `HTTP/2 lifecycle / domain model adversary`
- Finding: same-pull recv EOF/errorで`grpc_call_note_connection_broken()`を立てても、server streamingのterminal pathはincremental parserに1byteでも残っていれば`malformed_response_frame=true`を無条件に立てる。status priorityではmalformedの`INTERNAL`が`connection_broken`の`UNAVAILABLE`より先に評価される。同じraw fixtureに対してunaryは`call->stream_closed`の場合だけtruncated frameをmalformed化するため、TCP break途中の同一wire shapeがunary=`UNAVAILABLE(14)` / server streaming=`INTERNAL(13)`へ分岐する。
- Evidence: `src/server_streaming_call.c:367-374`、`src/unary_call.c:233-244`、`src/status_core.c:31-32,63-67`、fixture `:50057`の`writeRawGrpcPartialAndClose()`（response HEADERS + 3-byte partial gRPC header後にTCP close）。dev-sanitizer focused probe結果はunary `status=14, details=recv failed: Connection reset by peer`、server streaming `messages=0, status=13, details=recv failed: Connection reset by peer`。
- Expected model: clean HTTP/2 stream end(END_STREAM / stream close)の途中でgRPC frameが切れた場合はmalformed protocol responseとして`INTERNAL`。TCP/TLS connection breakでframeが途中になった場合はclient-observed connection failureとして`UNAVAILABLE`。call kindやGenerator pull境界で分類を変えず、response startedのためtransparent retryもしない。
- Why it matters: same-pull修正の主目的であるconnection failure taxonomyが、よくある「応答bytesの途中でpeer/socketが切れた」形ではserver streamingだけ未達となる。gax等のretry policyは`INTERNAL`と`UNAVAILABLE`を異なる扱いにする。
- Recommended fix: server streamingのtruncated-frame判定をunaryと同じく`call->stream_closed`（かつconnection breakでないterminal stream end）に限定する。fixture `:50057`でunary / server streaming双方のexact `UNAVAILABLE`、non-empty transport details、attempt 1回(no transparent retry)を固定し、別fixtureのclean END_STREAM mid-messageは`INTERNAL`を維持する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: dev-sanitizer PHP + current ASan/UBSan buildの`grpc.so`で両call kindを同fixtureへ実行し上記14/13差を再現。code-path reviewでpriority overrideを確認。
- Notes: 前回REVIEW-20260711-011の「complete message受信後のsame-pull break」はhelper追加で修正済み。本指摘はincremental parser途中stateが残るconnection breakに限定する。

### REVIEW-20260712-013: callback内RST fatalでunaryだけpolicy statusを返さずexceptionになる

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `HTTP/2 lifecycle / domain model adversary`
- Finding: response message-too-large等のcallback policyは先にcall-local validation flagを立て、その後`grpc_protocol_submit_rst_stream_in_callback()`がfatalを検出するとconnectionをdead化して`NGHTTP2_ERR_CALLBACK_FAILURE`を返す。server streaming outer loopはcall stateからstatusを解決するのでpolicy flag（例:`RESOURCE_EXHAUSTED`）が勝つが、unary outer loopは`nghttp2_session_mem_recv()`の任意のnegative returnでcall stateを捨て、`nghttp2_session_mem_recv failed` exceptionを投げる。そのためPHPT 038が明記する「policy status survives fatal submit」はserver streamingでしか成立しない。
- Evidence: `src/transport.c:936-952,2889-2900`、`src/unary_call.c:213-222`、`src/server_streaming_call.c:313-318,367-378`、`tests/phpt/038-fatal-rst-submit-marks-connection-dead.phpt:52-64`。focused probe (`GRPC_LITE_TEST_FAULT=rst-submit-fatal`, unary `grpc.max_receive_message_length=10`, 100-byte response)はstatus objectではなく`Exception: nghttp2_session_mem_recv failed`を再現した。
- Expected model: nghttp2 fatal後はsessionを再駆動せずcleanupをlocal-onlyにする一方、fatal前に確定したcall semantic/response-policy stateはcall kind共通のstatus priorityで解決する。message-too-large=`RESOURCE_EXHAUSTED`、malformed/compression=`INTERNAL`等がconnection/session failureより優先し、response startedなのでtransparent retryしない。
- Why it matters: rareなOOM pathでもofficial wrapper surfaceの通常status-return contractがunaryだけexceptionへ変わり、server streamingとtaxonomyが揃わない。今回導入したfault seamがこの不整合を決定的に再現できるのに、テストが片方しか覆っていない。
- Recommended fix: unaryのmem-recv fatal branchで、callbackが既にresponse-policy flagを確定した場合はdead cleanup invariantを守ったまま`build_unary_result`へ進み、そのflagからstatusを構築する。直接のnghttp2 parse/session fatalでpolicy flagがない場合のtaxonomyは別に保つ。PHPT 038へunary message-too-large caseを追加し、exact status、fresh connection、wire上RSTなし、attempt 1回をassertする。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: dev-sanitizer PHP + current ASan/UBSan buildの`grpc.so`でfocused unary probeを実行しexceptionを再現。server streaming policy caseは既存PHPT 038のexpected modelとcode pathを突合。
- Notes: fatal後のnghttp2 API禁止自体は現helper / dead cleanupで守られている。問題はsession lifecycleではなくcall-local error priorityとPHP surfaceの一貫性。

### REVIEW-20260712-014: test fault seamがproduction transportへ常時組み込まれ、ZTS-safeな初期化にもなっていない

- Severity: `High`
- Status: `Open`
- Reviewer role: `HTTP/2 lifecycle / domain model adversary`
- Finding: `GRPC_LITE_TEST_FAULT`は「test-only」とコメントされるがcompile-time gateがなく、productionのunary / server streaming submitと全RST pathが毎回このenvを評価する。envを設定すれば通常buildでも全callを人工的な`NGHTTP2_ERR_NOMEM`へ落とせる。さらにhelperはfunction-local static pointerをfirst call時にlazy setするためZTSの並行first-callで非atomic read/write data raceとなり、env値がある場合は`getenv()`の生pointerを保持してPHP `putenv()` / request shutdown後にdanglingになり得る。既存trace hookはまさにこの理由でMINIT時に値をcopyし、ZTS publication raceを避けているのに、新helperは「mirroring trace hook」としながらそのlifetime modelを満たさない。
- Evidence: `src/transport.c:887-900`、unconditional call sites `src/unary_call.c:155`, `src/server_streaming_call.c:127`, `src/transport.c:345,943`、`src/transport.h:91`。対照となる`grpc_lite_trace_cache_init()`は`src/transport.c:417-432`でMINIT / `strdup`の理由を明記。`config.m4`にtest-fault用macro/optionはなく、built `modules/grpc.so`にも`GRPC_LITE_TEST_FAULT`, `rst-submit-fatal`, `submit-request-fatal`文字列が存在する。`strstr`判定はcomma-separated tokenのexact matchでもない。
- Expected model: production transportのfailureはsocket/TLS/nghttp2から観測した事実だけが作り、test fault policyは明示的なtest build / injectable seamだけが所有する。ZTS production codeのprocess-global設定はMINITでsingle-threadedに確定・owned copyへ保存する。
- Why it matters: test harnessの内部概念がproduction RPC outcomeを直接支配し、ambient env誤設定で全RPCを停止できる。env未設定の通常ZTS起動でも最初の並行callがC data raceを踏むため、単なるhidden debug optionではなくproduction安全性の退行である。
- Recommended fix: `PHP_GRPC_LITE_ENABLE_TEST_FAULTS`等の明示macroでfault implementationとcall-site substitutionをcompile outし、PHPT/sanitizer専用buildだけ有効化する。通常PHPT suiteでも必要ならtest用extension artifactを分ける。どうしてもruntime envを残す場合でもMINITでexact token parseしたowned copyへ保存してZTS-safeにする必要があるが、production outcome mutation seamを残す判断は推奨しない。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: build/config/source audit、built moduleの`strings`確認。既存trace hookの明示的なenv lifetime commentと比較してrace / dangling modelを確認。
- Notes: `GRPC_LITE_TRACE_FILE`は観測だけを追加するdiagnosticであり、RPCを人工失敗させるfault seamとはproduction責務が異なる。

### REVIEW-20260712-015: strict preflight capのregressionはplaintextだけでTLS branchを固定していない

- Severity: `Low`
- Status: `Open`
- Reviewer role: `HTTP/2 lifecycle / domain model adversary`
- Finding: production fixは`recv()`だけでなく`SSL_read()`のrequested lengthもremaining capへclampしておりstaticには妥当だが、PHPT 035はh2c fixture `:50068`だけを使い、`wire.socket_preflight_read`累計だけをassertする。TLS persistent adoptionでOpenSSL内部にdecrypted bytesがpendingするbranchは、今回のstrict-max契約を直接固定していない。
- Evidence: `src/transport.c:1022-1073`、`tests/phpt/035-preflight-drain-cap-fallback.phpt:18-94`。TLS PHPT 030は通常TLS/mTLS callを検証するがpreflight cap / cancelled-stream backlogを作らない。
- Expected model: repositoryがproduction transportとしてTLS/mTLSを保証し、同じpublic INIがsocket/TLS両branchへ適用されるなら、strict byte capのregression evidenceも両branchを持つ。
- Why it matters: 将来socket側だけclampを維持してTLS側が64KiB scratch readへ戻っても、現suiteは全PASSする。現コードのprotocol safety defectではなく、新契約のbranch coverage gapである。
- Recommended fix: TLS raw HTTP/2 backlog fixture（またはSSL read seamを持つtransport unit）で小capを設定し、`wire.tls_preflight_read`の各requested length / 累計がcap以下、cap到達後はfresh connection、follow-up RPC OKを固定する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: PHPT 035をdev-sanitizer PHPで単独実行し1/1 PASS。trace assertionとfixture targetがplaintext branchだけであることをcode review。
- Notes: socket/TLS双方の現実装clamp自体はadequate。

## Review Result

- Blocker: none
- High: 1 Open (`014`) / Rejected history 1 (`001`)
- Medium: 2 Open (`012`, `013`) / 4 Fixed (`002`, `006`, `007`, `011`)
- Low: 3 Open (`004`, `008`, `015`) / 3 Fixed (`003`, `009`, `010`)
- Design Decision: 1 Accepted (`005`)
- HEAD `2af2d58`第四パス: strict capのsocket/TLS実装、fatal後session no-drive、pending DATA providerのdraining destructor lifecycle、complete-message same-pull connection breakのmarkerはadequate。dev-sanitizer PHPT 035は1/1 PASS。focused probesでunary direct breakはUNAVAILABLEを確認したが、同じpartial-frame TCP breakはserver streamingでINTERNALへ上書きされ、unary callback-policy RST fatalはstatusではなくexceptionになった。test fault seamのproduction/ZTS boundaryをHighとして追加。current docs map、microsecond ordering oracle、TLS strict-cap coverageのLowも残るため、required domain gateは未通過。
