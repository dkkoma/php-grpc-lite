# PR #29 deadline RST_STREAM connection温存 敵対的C安全性レビュー 2026-07-11

## Scope

- PR #29: base `e49d4be` から HEAD `2af2d58` まで（今回の第四パス再レビュー範囲 `be1b97e..2af2d58`、実装修正commit `d54a2ba`）
- 元issue: `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md`
- `src/transport.c` (`cancel_grpc_call_stream`, `send_pending_h2_frames_with_deadline`, stream registration/owner cleanup, persistent preflight)
- `src/unary_call.c` (deadline read timeout と local cancel 後のcleanup)
- `src/server_streaming_call.c` (deadline/user cancel、resource destroy、既存in-flight streamのnext loop)
- `src/grpc_exchange_state.h`, `src/transport.h`
- `tests/phpt/033-deadline-rst-stream-connection-reuse.phpt`、`034-dead-connection-terminal-for-owners.phpt`、`035-preflight-drain-cap-fallback.phpt`、`036-draining-connection-cancel-sends-rst.phpt`、`037-draining-destructor-pending-request-data.phpt`、`038-fatal-rst-submit-marks-connection-dead.phpt`、`039-fatal-submit-request-marks-connection-dead.phpt`、`poc/test-server/main.go` のraw lifecycle fixtureおよびsanitizer runner

## Reviewer Role

- `low-level C / nghttp2 transport safety adversary`

## Review Prompt Summary

- 元issueの範囲内で、ownership/lifetime、use-after-free、callback reentrancy、nghttp2 return value、errno/SSL error、connection-dead fallback、partial write、stale per-call state、`locally_cancelled`、deadline arithmetic、全変更callerを敵対的に確認した。
- 特に、coalesced RST flushの実wire writeと`on_stream_close_callback`の順序、RST失敗時にin-flight survivorが残る場合、local reset後のpending inbound backlogを追跡した。

## Issues

### REVIEW-20260711-001: nghttp2 direct fatal returnをdeadへ畳み込めずsession APIを継続できる

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `low-level C / nghttp2 transport safety adversary`
- Finding: `b13362b` はdead connectionのunregisterをlocal-onlyにし、SPECも「dead後は `nghttp2_session_del()` 以外のsession APIを呼ばない」とglobal invariantを明記したが、deadへ遷移させないdirect fatal returnが2群残る。(a) unary/server-streamingの `nghttp2_submit_request()` は負のstream idを一括failureにするだけで、`nghttp2_is_fatal(rv)` を判定しない。`NGHTTP2_ERR_NOMEM` ならsessionはfatalなのにconnectionはusable/cachedのままである。(b) response frame callback/parserからの `nghttp2_submit_rst_stream()` はreturnを無視する。`NGHTTP2_ERR_NOMEM` 後もcallbackは0を返すかparserを継続し、同じ `nghttp2_session_mem_recv()` 内の後続callback、stream-user-data getter、外側の `nghttp2_session_want_write()` / `nghttp2_session_send()`へ進み得る。deadにならないため、今回追加したunregister guardはこの2群に効かない。call site自体は本PR以前から存在するが、本PRが完了を主張するglobal dead/fatal invariantの適用漏れとしてscope内である。
- Evidence: nghttp2 1.64.0 `nghttp2.h` の `NGHTTP2_ERR_FATAL` commentは、fatal codeを受けたapplicationはそのsession objectの使用を停止し、許可される唯一の操作は `nghttp2_session_del()` と規定する（installed header `nghttp2/nghttp2.h:444-449`）。判定APIは同header `:5875-5881` の `nghttp2_is_fatal()`。submit requestの未判定は `src/unary_call.c:155-161` と `src/server_streaming_call.c:127-140`。RST return無視は代表的に `src/transport.c:2219-2226`、同型response parser群 `:2704-2869`、read-ahead/metadata limit `:2943-2952,2994-3011`。fatal callbackを中断しない場合、getterは `src/transport.c:100-105`、want-writeは `src/unary_call.c:215` / `src/server_streaming_call.c:326` / `src/transport.c:1004`、sendは `src/transport.c:1875` に到達可能である。新規callまで戻ればregister setter `src/transport.c:108-114` もfatal sessionへ触れる。
- Expected model: direct nghttp2 returnが `nghttp2_is_fatal(rv)` を満たした時点でconnectionをterminal deadへ遷移させ、現在のAPI stackを即unwindし、以後はowner/list/countのlocal bookkeepingと最終 `nghttp2_session_del()` だけを許す。`dead` をこのconservative terminal stateとして統一すれば、別の `session_fatal` stateは不要である。
- Why it matters: nghttp2が「processing was terminated」と宣言したsessionへgetter/setter/submit/send/recv/queryを続けるlibrary contract違反である。OOMなど稀なerror pathでも、persistent cache経由の再利用、callback中の不整合state参照、worker crashまたはC memory safety failureへつながり得る。
- Recommended fix: (1) unary/server-streamingのsubmit request returnを一旦 `rv` へ保持し、負値かつ `nghttp2_is_fatal(rv)` なら `mark_connection_dead(connection, rv)` と通常のunusable cache detachを行ってからcall/request cleanupへ進む。(2) response-side RST submitをchecked helperへ集約し、fatalならconnectionをdeadにして `NGHTTP2_ERR_CALLBACK_FAILURE` をcallback chainから返し、外側 `nghttp2_session_mem_recv()` を即失敗させる。callback内からsession send/flushはしない。(3) getter/registerにdead guard、unary/server-streamingのmem_recv直後にもI/O gateを置き、将来のcallback-side terminal transitionに対するdefense-in-depthを追加する。(4) submit request fatalとcallback内RST fatalをfault injectionし、fatal return以後にsession APIが `nghttp2_session_del()` 以外0回であることを固定する。
- Fix summary: `0480479` は `connection_io_allowed()` を追加してdead connectionのwire/session再駆動を拒否し、`b13362b` はdead時のunregisterをlocal bookkeepingだけにした。`d54a2ba` は残っていた2群を閉じた。(a) unary/server-streamingの `nghttp2_submit_request()` negative returnを `nghttp2_is_fatal()` で判定してdead化、(b) response callback/parser内のRST submit 14 callerをchecked helperへ統一し、fatal時はdead化して `NGHTTP2_ERR_CALLBACK_FAILURE` を最外callbackへ返し、`nghttp2_session_mem_recv()` を即時unwindする。
- Fix commit: `0480479`, `b13362b` (partial), `d54a2ba` (fatal call-site remediation)
- Verification: HEAD `2af2d58` でproductionの `nghttp2_submit_rst_stream()` は `cancel_grpc_call_stream()` と `grpc_protocol_submit_rst_stream_in_callback()` の2箇所だけになった。後者はresponse parser / metadata / read-ahead / PUSH_PROMISEの14 callerすべてからreturnが最外のregistered callbackまで伝播し、fatalなら `mark_connection_dead()` + `NGHTTP2_ERR_CALLBACK_FAILURE` により `nghttp2_session_mem_recv()` が即時失敗する。unary / server streamingの `nghttp2_submit_request()` もnegative returnを `nghttp2_is_fatal()` で判定してdead化する。外側はmem_recv negative branchでcleanupへ入り、`nghttp2_session_want_write()` に進まず、dead時のunregisterは `b13362b` のlocal-only処理なので、今回の2 fault seamからfatal後に `nghttp2_session_del()` 以外へ到達するproduction pathはstatic call-path review上なくなった。`git diff --check be1b97e..2af2d58` はPASS。ただしPHPT 038/039の観測はwire frameとcache reuseであり、session API呼出し0回を直接計測しない点は `REVIEW-20260711-007`、unary fatal branchの即時cache detach漏れは `REVIEW-20260711-006` として分離した。
- Notes: lifecycle safetyのために別の `session_fatal` flagは必須ではない。`dead` を「socket/TLS I/Oだけでなく `nghttp2_session_del()` 以外のsession-object APIを二度と呼ばないconservative terminal state」と定義すれば十分である。具体的には、submit requestの負値をcaptureして `nghttp2_is_fatal(rv)` なら即 `mark_connection_dead()`（および通常のunusable cache detach）してからcall cleanupへ進む。response-side RSTはchecked helperへ集約し、fatalならdead化した上でcallbackから `NGHTTP2_ERR_CALLBACK_FAILURE` を返して外側 `nghttp2_session_mem_recv()` を即unwindする。`grpc_call_from_stream_id()` と `register_grpc_call_stream()` のdead guard、およびmem_recv直後のI/O gateもdefense-in-depthとして併せて閉じるべきである。

#### Fatal nghttp2 API follow-up audit (HEAD `be1b97e`)

| API / production anchor | fatal return | 現在の後処理 | 必要な対応 |
|---|---|---|---|
| `nghttp2_session_callbacks_new` (`src/transport.c:401`) / `nghttp2_session_client_new` (`:1623`) | `NGHTTP2_ERR_NOMEM` | callbacks/session生成前で、output pointerは未設定のまま即destroy | 現行で十分。fatalになったlive session自体がない |
| `nghttp2_submit_settings` (`src/transport.c:1636`) / `nghttp2_submit_window_update` (`:1643`) | `NGHTTP2_ERR_NOMEM` | setup中のconnectionを即 `destroy_h2_connection()` し、sessionへの次操作は `nghttp2_session_del()` | 現行で十分 |
| `nghttp2_submit_request` (`src/unary_call.c:155`, `src/server_streaming_call.c:127`) | `NGHTTP2_ERR_NOMEM` | 負値を一括エラーにするだけでconnectionをdeadにせず、cache/sessionを再利用可能なまま残す | returnを `rv` へcaptureし、`nghttp2_is_fatal(rv)` なら必ずdead化 + cache detach。unaryはwrapperのFAILURE early return (`src/wrapper_adapter.c:516-518`) が通常のunusable removalへ到達しないためcore内でdetach/destroyを完結させる |
| `nghttp2_submit_rst_stream` in `cancel_grpc_call_stream` (`src/transport.c:345`) | `NGHTTP2_ERR_NOMEM` | 非0をdead化し、`b13362b` 後はowner cleanupもlocal-only | 修正済み |
| response callback/parser内の `nghttp2_submit_rst_stream`（代表: `src/transport.c:2223`; 同型parser群 `:2709-2869,2950,3008`） | `NGHTTP2_ERR_NOMEM` | returnを全て無視し、fatal化したsessionで同じ `nghttp2_session_mem_recv()` の処理と後続getter / `want_write/send` を続け得る | submit-only checked helperに集約。fatal時はdead化し、callback chainに `NGHTTP2_ERR_CALLBACK_FAILURE` を返して外側 `mem_recv` を直ちに中断する（callback内からflush/session_sendはしない） |
| `nghttp2_session_send` (`src/transport.c:1875`) | `NGHTTP2_ERR_NOMEM`, `NGHTTP2_ERR_CALLBACK_FAILURE` | helper内でdead化し、`b13362b` 後のruntime owner cleanupはlocal-only。setupは即destroy | 修正済み |
| `nghttp2_session_mem_recv` (`src/unary_call.c:204`, `src/server_streaming_call.c:319`, `src/transport.c:995`) | `NGHTTP2_ERR_NOMEM`, `NGHTTP2_ERR_CALLBACK_FAILURE`, `NGHTTP2_ERR_BAD_CLIENT_MAGIC`, `NGHTTP2_ERR_FLOODED` | 負値はdead化済み。preflightはownerなしでdestroy、unary/streamingはowner cleanupへ進む | dead後のunregisterは修正済み。callback内RST submit失敗を必ずfailureとして外側へ伝播する |
| `nghttp2_session_want_write` (`src/unary_call.c:215`, `src/server_streaming_call.c:326`, `src/transport.c:1004`) | 自身はfatalを返さない | preflightだけはmem_recv後に `connection_usable()` を再確認。unary/streamingはsuccessful mem_recv直後に無条件で呼ぶ | callback内fatalをmem_recv failureへ伝播する。併せてmem_recv直後のdead guardをunary/streamingにも置けば将来のcallback-side dead transitionを防御できる |
| `nghttp2_session_get/set_stream_user_data` (`src/transport.c:100-114,126-153`) | setter自身はnonfatal `NGHTTP2_ERR_INVALID_ARGUMENT` のみ | unregister (`:146-153`) はdead時にskipするよう修正済み。一方、getter (`:100-105`) / register (`:108-114`) にはdead guardがない | cleanup setterは修正済み。getter/registerにもdead guardを置き、callback継続や将来のbypassを防ぐ |

Safetyのために別の `session_fatal` flagは不要である。`dead` を「wire I/Oだけでなく、`nghttp2_session_del()` 以外のsession-object APIを二度と呼ばないconservative terminal state」として一貫させれば、TCP EOF / partial flush / fatal nghttp2を同じcleanupに畳み込める。fatalかどうかで診断を分けたい場合はerror-source fieldを追加してもよいが、lifecycle safetyの分岐には使わない。ただし一律skipを有効にする前提として、上記の未処理fatal returnをすべてdeadへ遷移させ、callback内のfatalは外側APIへ伝播して処理を即中断させる必要がある。

### REVIEW-20260711-002: local RST後の64KiB preflight backlogをconnection failureとして捨てるかが未明示

- Severity: `Design Decision`
- Status: `Fixed`
- Reviewer role: `low-level C / nghttp2 transport safety adversary`
- Finding: local RSTのwire送出に成功しても、serverが既に送ったclosed-stream frameが次adoption時に64KiB以上pendingなら、`drain_pending_connection_data_for_reuse()` は `GRPC_LITE_PREFLIGHT_DRAIN_MAX_BYTES` 到達を理由にconnectionをdrainingへ落とす。scratch bufferと上限がともに65536 bytesなので、最初のreadがちょうど65536 bytesを返した場合は、次のnonblocking readでEAGAIN境界を確認する前に必ずcap-hitとなる。したがってprotocol上安全に無視できるlocal-reset済みstreamのbacklogだけでも、次callは新規connectionとなり `persistent_reused=false` になる。
- Evidence: `src/transport.c:14-16` のpreflight上限、`src/transport.c:902-987` `drain_pending_connection_data_for_reuse()`、`src/transport.c:1039-1052` `preflight_persistent_connection()`、`src/transport.c:1637-1687` `get_persistent_connection()`。本PRのSPEC/issueはclosed-stream frameをnghttp2が安全に処理し、到着済みbytesをpreflight drainするとしてconnection reuseを説明しているが、cap-hit時のbest-effort fallbackは明記していない。
- Expected model: 現挙動はprotocol safety上は保守的であり、connectionを捨てること自体は安全である。一方、元issueの「RST送出後にpersistent cacheへ残して次callで再利用する」を必須保証とするなら、単にbacklog量が64KiBへ達したことをconnection failureと同一視してはならない。reuseをbest-effort policyとするなら、cap-hit時は安全性ではなくbounded preflightのpolicy fallbackであることをSPEC/issueへ明記する。
- Why it matters: large/fast responseがdeadline直前まで送信された実運用では、RST自体が成功しても毎回TCP/TLS handshakeへ戻り、issueが狙うworker latency改善を得られない。現コードは安全側なのでcorrectness障害ではないが、目標の保証範囲とテスト期待が曖昧なままになる。
- Recommended fix: 次のどちらかを明示的に選ぶ。(A) reuseはbest-effortで、preflight 64KiB/64 iteration cap到達時はconnectionを捨てるとSPEC/Decision Logへ記録し、capちょうど・cap超過backlogのtestでpolicyを固定する。(B) local-reset後のbacklogでもconnection温存を保証するなら、GOAWAY等のconnection control frameを見落とさずに残りbytesを処理してから新streamをsubmitできるbounded adoption設計を追加する。単にcap-hitを成功扱いして未処理bytesを残すだけでは、残りにGOAWAYがある場合の新stream submissionを防げない。
- Fix summary: SPEC §4.2にreuseはbest-effortであり、64KiB / 64 iterationsのcap到達時はconnectionをdrainingにして新規接続へfallbackするpolicyを明記。PHPT 035でlarge cancelled-stream backlog後の `persistent_reused=false`、preface 2回、follow-up STATUS_OKを固定した。
- Fix commit: `0480479`
- Verification: HEAD `6795a5a` のSPEC / PHPT 035 static reviewで、policyとtest expectationが一致することを確認。repository記録はPHPT 035を含む20/20 PASS。
- Notes: これは現時点のprotocol/memory safety defectではなく、connection reuse保証とbounded preflight policyの設計判断として分類した。

### REVIEW-20260711-003: draining上のadmitted streamをcancel/destroyするとpending DATA sourceを解放後に別ownerが再参照できる

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `low-level C / nghttp2 transport safety adversary`
- Finding: `connection_io_allowed()` がdrainingを許可する分離自体は正しいが、stream cancel / resource destroyは依然 `connection_usable()` をgateに使う。そのためGOAWAYの `last_stream_id` 以下でadmit済みのstreamでも、`src/transport.c:329` の `cancel_grpc_call_stream()` はRSTをsubmitせず、`src/transport.c:363` のdestructorもcancelをskipする。そのままowner cleanupが `server_streaming_call_state` / `state->request` を解放する一方、nghttp2 streamが保持するoutbound DATA providerの `source.ptr = &state->call` はstream closeなしで残り得る。同じdraining connectionの別のadmitted ownerは新設 `connection_io_allowed()` によって `nghttp2_session_send()` を正当に再駆動できるため、`h2_send_data_callback()` が解放済み `grpc_call` / request bytesを参照するuse-after-freeに至る。`server_streaming_call_cancel_resource()` も `src/server_streaming_call.c:419` の同じgateにより、draining上ではuser cancel自体がno-opになる。
- Evidence: 到達可能な順序は (1) server-streaming Aをinitial stream windowより大きなrequestでopenし、flow controlによりoutbound DATA providerをpendingのまま残す（`src/server_streaming_call.c:124-127`）、(2) 同一connectionでstream Bもopen、(3) peerがA/BのID以上の `last_stream_id` でGOAWAYを送り両方をadmitしたままconnectionをdraining化、(4) Aをcancelまたはresource destroyするが `connection_usable()==false` でRST/closeをskipし、`clear_connection_server_streaming_call_state_owner()` 後にstate/requestをfree、(5) peerがWINDOW_UPDATEまたはBのframeを送り、Bの次pullが `src/server_streaming_call.c:260,276,314-315` からsessionを駆動、(6) nghttp2がAのpending DATAをsendしようとし、`src/transport.c:1945,1971-1981` でfreed `source->ptr` / requestをdereference。`src/transport.c:1934-1941` の既存lifetime commentは「dead/drainingならsession_sendは二度と呼ばない」とするが、新しいpredicateは意図的にdraining上のI/Oを許しており、commentと実際lifecycleが直接矛盾する。
- Expected model: `draining` は新規stream adoptionだけを禁止し、admit済みstreamの通常I/Oとstream-scoped cancel/RSTは許可する。call/request lifetimeは「stream closeによりnghttp2がoutbound DATA itemをdetach」または「connectionがdeadで今後session driveなし」のどちらかを満たすまで終了させない。draining単体はその代替条件にならない。
- Why it matters: peerがGOAWAY / flow-control frameの順序を制御し、PHP userlandが正常なstream resource cancel/destroyを行うだけで、worker process内のheap use-after-freeに至り得る。またmemory safetyが顧在化しないsmall requestでも、draining上の `cancel()` が効かずpeer側streamを不要に存続させる。
- Recommended fix: `cancel_grpc_call_stream()`、`destroy_server_streaming_call_state()`、`server_streaming_call_cancel_resource()` のstream-local close gateを `connection_usable()` から `connection_io_allowed()` へ分離し、draining上のadmit済みstreamにはRST submit/flushを許す。RST/flushが成功すればstream closeがDATA itemをdetachした後にfree、失敗すればconnectionをdeadにして今後のsession driveを禁止した後にfreeする。あわせて `h2_send_data_callback()` のlifetime commentからdrainingをterminal条件とする記述を除く。deterministic regressionにはraw fixtureでsmall initial stream windowをadvertiseし、Aのlarge request DATAをpending化、A/BをadmitするGOAWAY、A destroy後のWINDOW_UPDATE、Bの存続frameを順に送る。ASan/UBSan下でUAFなし、AへのRST、Bの完走、connectionの最終destroy 1回を固定する。
- Fix summary: `b13362b` は `cancel_grpc_call_stream()`、`destroy_server_streaming_call_state()`、`server_streaming_call_cancel_resource()` のstream-local close gateを `connection_usable()` から `connection_io_allowed()` へ変更した。これによりGOAWAY-drainingでもadmit済みstreamはRST submit/flushを実行する。成功時はnghttp2のstream closeでpending DATA providerをdetachしてからcall/requestをfreeする。RST submit failureまたはflush failure時はconnectionをdeadにし、`b13362b` のlocal-only unregisterと組み合わせて、call/request free後に別ownerがsessionを再駆動する経路を禁止する。user cancel成功後はcompleted化とowner clearが先に完了し、その後のdestructorは再cancelせず一度だけstateを解放する。明示cancelなしのresource destructorもdraining中に同じcancel helperを通る。
- Fix commit: `b13362b`
- Verification: HEAD `be1b97e` のstatic ownership/lifetime reviewで、明示cancel、直接destructor、RST submit failure、RST flush failure、user cancel後destructorの各分岐に「stream closeまたはterminal deadの後にfree」という順序が成立することを確認。DockerでPHPT 036を再実行しPASS、関連するPHPT 034/036の独立実行は `2/2 PASS`。fixture `:50067` はresponse messageと `GOAWAY(last_stream_id=2^31-1)` を送りstreamをopenのまま維持し、PHPT 036はdraining上の明示cancelがwireへRSTを送ることを固定している。
- Notes: PHPT 036はgate regressionには有効だが、small request / single stream / explicit cancelであり、元findingのpending outbound DATA provider、別ownerによるsession再駆動、明示cancelなしdestructor、RST submit/flush failureは再現しない。また `h2_send_data_callback()` 直前のlifetime commentはなおdrainingをterminal条件と誤記している。この残存はproduction code defectではなく、次のLow verification/invariant findingとして分離する。

### REVIEW-20260711-004: DATA providerのlifetime invariantとsanitizer regressionがdrainingの実modelを固定していない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `low-level C / nghttp2 transport safety adversary`
- Finding: `h2_send_data_callback()` 直前のlifetime commentは、call解放を許す条件を「stream closedまたはconnection dead/draining」と説明し、drainingでは `nghttp2_session_send()` を二度と呼ばないとしている。しかし実modelはdraining上のadmit済みownerにsession I/Oを許し、本修正もstream-local RST closeを必須にした。code pathは修正済みだが、局所invariantがなお誤っている。加えて新設PHPT 036はGOAWAY-draining上で明示 `cancel()` がRSTをwireへ出すことだけを固定し、修正対象だったmemory-safety shapeを直接通らない。requestは小さい `BenchRequest` でDATA providerをflow-control pendingにせず、connection上のstreamも1本だけで、resource destructorではなく明示cancelを使う。このためcall/requestを解放した後に別のadmitted ownerがsessionをdriveしてDATA callbackを再発火する条件がない。sanitizer runnerはfixture port 50067/50068/50069をpreflightへ追加し全PHPTをASan/UBSan/TSan対象へ含めるが、scenario自体にこのlifetime pressureがないので、修正が将来退行してもsanitizerがUAFを検出する保証はない。
- Evidence: `src/transport.c:1957-1963` はdrainingをdeadと同じterminal条件として列挙する一方、`connection_io_allowed()` とserver-streaming next loopはdraining I/Oを許す。`tests/phpt/036-draining-connection-cancel-sends-rst.phpt` は単一のserver-streaming callへsmall `BenchRequest` を渡し、GOAWAY観測後に `$call->cancel()` を呼ぶ。`poc/test-server/main.go` のport 50067 fixtureはmessage + GOAWAYを送ってstreamをopenに保つが、small initial stream window、2本目のadmitted stream、WINDOW_UPDATEによる再drive、destructor-only path、RST failure injectionを持たない。`tools/test/check-c-sanitizer.sh` と `tools/test/check-phpt.sh` の変更はport preflightとsuite inclusionであり、これらの状態遷移を追加しない。
- Expected model: regression testは、nghttp2がcall-owned DATA sourceを保持している状態でcallをcancel/destroyし、その後も同一draining connectionを別ownerが駆動するというownership境界を作る。修正が正しければAのDATA providerはRST closeでdetachされるか、failure時にconnectionがterminal deadとなり、Aのstate/request free後にcallbackが再発火しない。
- Why it matters: 現code fixはstatic review上安全だが、誤った局所commentは将来のcleanup追加でdrainingをfree条件として再利用させ得る。UAF防止の本質は3箇所のgateとdead/unregister契約に跨り、どれか1箇所が `connection_usable()` へ戻る、またはfailure cleanup順序が変わると、現PHPT 036はwire RSTの一部退行しか捕捉せずmemory-safety退行を見逃し得る。
- Recommended fix: lifetime commentからdrainingをterminal条件として除き、「stream closeでDATA itemをdetach、またはdeadで今後session driveなし」の二条件へ直す。raw fixtureでは小さいinitial stream windowをadvertiseし、Aへwindowより大きいrequestを送ってoutbound DATA providerをpending化し、Bも同じconnectionへadmitした後、A/B以上の `last_stream_id` でGOAWAYを送る。Aを明示cancelせずdestroyするvariantと、明示cancel後にdestructorを通すvariantを作り、その後peerのWINDOW_UPDATEまたはBのframeでsessionを再driveする。ASan/UBSan下でUAFなし、AへのRST、B完走を固定する。RST submit/flush fault injectionも可能なら同fixture/test helperで別variantとしてdead後の再driveなしを固定する。
- Fix summary: `d54a2ba` は `h2_send_data_callback()` のlifetime commentを「stream closeでDATA itemをdetach、またはdeadで今後session driveなし」の二条件へ修正した。fixture `:50070` はserverの `SETTINGS_INITIAL_WINDOW_SIZE=1024` によりAの262144-byte request DATAをpending化し、A/Bの2 streamをadmitした `GOAWAY(last_stream_id=2^31-1)` 後にAを明示cancelせずdestructorで解放する。その500ms後にAへ `WINDOW_UPDATE` を送り、Bのmessage/trailersで同じdraining sessionを再駆動する。PHPT 037はAへのCANCEL RST、GOAWAY後の送出順序、Bの2 message完走とSTATUS_OKを固定し、sanitizer suiteにも含まれる。
- Fix commit: `d54a2ba`
- Verification: HEAD `2af2d58` のfixture / PHPT / cleanup順序をstatic reviewした。Aはdefault 65535-byte stream windowより大きいrequestなのでserver SETTINGS適用前に送信が始まってもDATA providerが残り、Bはsmall requestでEND_STREAMへ到達する。A destructorはdraining上でも `cancel_grpc_call_stream()` を通り、RST submit/flush成功ならnghttp2 stream close後、failureならterminal dead後にstate/requestをfreeする。Bのdelayed pullがその後sessionを駆動するため、元UAF shapeを直接通す。独立Docker結果は第四パスのtest reviewer結果と統合する。
- Notes: PHPT 036はdraining gateとwire RSTの回帰testとしては有効であり、削除ではなくownership-focused sanitizer caseの追加を求める。

### REVIEW-20260711-005: fault injection seamがproductionへ漏れ、request越しのdangling getenv pointerとZTS data raceを作る

- Severity: `High`
- Status: `Open`
- Reviewer role: `low-level C / nghttp2 transport safety adversary`
- Finding: `d54a2ba` の `grpc_lite_test_fault_enabled()` はtest専用とコメントされる一方、compile-time guardなしで通常のextensionへ常時組み込まれ、最初のRPCで `getenv("GRPC_LITE_TEST_FAULT")` が返したraw pointerをfunction-staticへ保存する。同じ `src/transport.c` のtrace設定は、PHP `putenv()` のrequest shutdown復元時に `getenv()` pointerがfreeされ得ることと、lazy runtime初期化にはZTS publication raceがあることを明記し、MINITで値をcopyしている。新しいfault hookはその両方を避けていない。PHP userlandが最初のRPC前に `putenv()` でfaultを設定した場合、同request内の再設定またはRSHUTDOWN後にstatic pointerがdanglingになり、次のRPCの `strstr()` がuse-after-freeする。ZTSでは最初の複数RPCが `faults` を同期なしにread/writeするC data raceにもなる。またnormal buildのPHP userlandから意図的に `NGHTTP2_ERR_NOMEM` を注入でき、worker全体へprocess lifetimeで残るtest control surfaceになっている。
- Evidence: `src/transport.c:417-421` はtrace env pointerのinvalidationとZTS理由を明記し、`:425-433` はMINIT時copyを実装する。一方、`:887-900` はraw `getenv()` pointerをlazy function-staticへ保存して `strstr()` する。`config.m4` にはtest-fault build option/defineがなく、通常の `./configure --enable-grpc` で作られた `modules/grpc.so` に対する `strings` でも `GRPC_LITE_TEST_FAULT`, `rst-submit-fatal`, `submit-request-fatal` を確認した。`src/unary_call.c:155-157`、`src/server_streaming_call.c:127-129`、`src/transport.c:345-348,943-946` がproduction request/cancel/parser pathから直接このseamを呼ぶ。ASan buildのPHP built-in serverで、request 1が `putenv('GRPC_LITE_TEST_FAULT=submit-request-fatal')` 後にRPCを実行し、RSHUTDOWNを挟んだrequest 2が通常RPCを実行する2-request probeを行うと、request 2は `src/transport.c:899` の `grpc_lite_test_fault_enabled()` でheap-use-after-freeを確実に検出した。ASanのfree stackは `php_putenv_destructor` -> `zend_hash_destroy` -> `zm_deactivate_basic` -> `php_request_shutdown`、allocation stackは `zif_putenv` だった。token判定もcomma-separatedと説明しながらexact parseではなくsubstring `strstr()` であり、別processの `GRPC_LITE_TEST_FAULT=not-submit-request-fatal` probeでも `nghttp2_submit_request failed` が発生した。
- Expected model: fault injectionはproduction transportのruntime responsibilityではない。通常buildにはseamも環境変数も存在させず、明示的なtest build defineでのみ有効化する。test buildでも設定はsingle-threaded MINITでowned copyへ取り込み、MSHUTDOWNで解放し、comma tokenをexact matchする。少なくともnormal buildへ残す判断をするなら、trace hook同様のMINIT-owned immutable stateでUAF/ZTS raceを除き、production diagnostic surfaceとしてSPECへ明示する。
- Why it matters: long-lived FPM / FrankenPHP / ZTS workerでrequest境界を越えるheap use-after-free、未定義動作、worker crash、またはfaultが意図せず別requestへ固定されるcross-request DoSになる。これはテスト支援コードがproduction memory-safetyとtransport availabilityを壊す境界違反である。
- Recommended fix: `--enable-grpc-test-faults` 等のdefault-off configure optionと `PHP_GRPC_LITE_ENABLE_TEST_FAULTS` guardを設け、PHPT/coverage/sanitizer/ZTSのtest buildだけで有効化する。hook stateはMINITでcopyしMSHUTDOWNでfree、exact comma-token parserを使う。normal buildではhelperを常にfalseへcompileするかcall site自体を除去する。長寿命workerで `putenv()` を変更してもnormal buildへ影響せず、test buildでも再設定/RSHUTDOWN後にUAFしない回帰を追加する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: HEAD `2af2d58` のmodule lifecycle、config、normal module binary、全4 call siteをstatic監査。`git diff --check be1b97e..2af2d58` はPASS。Docker `dev-sanitizer` の現在moduleで同一PHP processのreal RINIT/RSHUTDOWNを通す2 HTTP request probeを実行し、2件目でASan `heap-use-after-free`（READ size 1、`grpc_lite_test_fault_enabled`, `src/transport.c:899`）を再現した。別ASan processに `GRPC_LITE_TEST_FAULT=not-submit-request-fatal` を渡すprobeはsubstring誤発火による `nghttp2_submit_request failed` を再現した。PHPT 038/039の `--ENV--` はprocess startup前に値が入り1 process内で完結するため、このlong-lived worker / ZTS defectを通らない。ZTSのlazy-static data raceはruntime probe未実施だが、MINIT前初期化やatomic/thread-local/module-global同期がないことをsource上確認した。
- Notes: fault seamそのものの必要性は妥当だが、「test-only」というcommentだけではproduction boundaryにならない。

### REVIEW-20260711-006: unary submit-request fatalはdead cache entryを即時detachせず、異なるkeyでcache capacityを消費し続ける

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `low-level C / nghttp2 transport safety adversary`
- Finding: unaryの `nghttp2_submit_request()` fatal branchはconnectionをdeadにするためsession再利用は防げるが、そのconnectionをpersistent cacheからdetachしない。`clear_connection_call_owner()` はstream bookkeepingだけでcacheを扱わず、wrapperはcoreのFAILUREを受けるとunusable removalをせず即returnする。次に同じkeyを使えば `get_persistent_connection()` がlazy evictionするのでPHPT 039は通るが、異なるchannel keyを使うfatalが続くとdead entryはcacheに残り、128 slotsを埋めて以後のhealthy keyを `persistent connection cache limit exceeded` にする。server streamingの同じfatal branchは `destroy_server_streaming_call_state()` -> `clear_connection_server_streaming_call_state_owner()` がunusable connectionをdetachするため非対称である。
- Evidence: fatal markは `src/unary_call.c:155-163`、直後のcleanup/FAILUREは `:164-168`。`src/transport.c:300-310` の `clear_connection_call_owner()` はdetach/destroyを行わない。`src/wrapper_adapter.c:516-518` はFAILURE時に `remove_unusable_persistent_connection()` を呼ばずreturnする（diagnostic wrapperは `src/diagnostic/bench.c:1878-1883` で明示的にremoveする）。cacheは `src/transport.c:1764-1769` で同じkeyを再取得した時だけdead entryをlazy evictionし、`:1782-1785` は全key合計が `GRPC_LITE_MAX_PERSISTENT_CONNECTIONS` (128)に達すると新規connectionを拒否する。PHPT 039は同じclient/keyを3回使うため各次attemptが直前entryを掃除し、このleakを観測しない。
- Expected model: fatal nghttp2 returnでterminal deadになったpersistent connectionは、callがownerを持つか否かにかかわらず、そのfailure cleanup内でcacheからdetachする。cacheはadopt可能なconnectionだけをcapacityとして保持し、同じkeyの将来accessをcleanup triggerにしない。
- Why it matters: real `NGHTTP2_ERR_NOMEM` は稀でも、terminal resourceをcacheへ残すとmemory/fdをrequestまたぎで保持し、複数target/authorityを使うworkerではcache-wide availability failureへ増幅する。現在の常時有効fault seamを使えばPHP userlandから決定的に再現できる。
- Recommended fix: unary fatal branchで通常のunusable detach + unowned destroyを完結させるか、wrapperのcore FAILURE branchをdiagnostic wrapperと同様に `remove_unusable_persistent_connection(key, h2)` してからreturnさせる。異なるconnection keyでfatalを繰り返してもdead entry countが増えず、後続healthy keyを作れるtestを追加する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: HEAD `2af2d58` のunary error/owner/cache caller chainをstatic reviewし、same-key lazy evictionとdistinct-key retentionを確認した。さらにDocker `dev-sanitizer` でprocess-startup envに `GRPC_LITE_TEST_FAULT=submit-request-fatal` を設定し、同じtargetへ `grpc.default_authority` だけを変えた129 unary callsを実行した。index 0..127は128回すべて `nghttp2_submit_request failed`、index 128はstatus code 14 / details `persistent connection cache limit exceeded` となり、dead entriesが128-slot capacityを占有することを動的に再現した。ASan/UBSan reportはなかった。
- Notes: session-object APIをfatal後に呼ぶ経路ではないため、修正済みの `REVIEW-20260711-001` からcache ownership/capacity defectとして分離した。

### REVIEW-20260711-007: fatal fault PHPTはwire/cache結果を検証するが「fatal後session API 0回」を直接固定しない

- Severity: `Low`
- Status: `Open`
- Reviewer role: `low-level C / nghttp2 transport safety adversary`
- Finding: PHPT 038/039はfault後にstream frameがwireへ出ないこと、same-keyの次callがfresh connectionを使うこと、status taxonomyが保たれることを確認する。一方、nghttp2 fatal contractの核心である「fatal return後は同じsessionへ `nghttp2_session_del()` 以外を一度も呼ばない」は計測していない。`nghttp2_session_get/set_stream_user_data()` や `nghttp2_session_want_write()` のようにwire eventを出さないAPIがfatal後に呼ばれても両testはPASSし得る。またPHPT 038のcallback-policy fatalはtest末尾の最後のcallであり、そのdead connectionの後続adoptionも確認しない。現在のcodeはstatic call-path review上即時unwindできているが、global invariantのregression gateとしては観測が弱い。
- Evidence: `tests/phpt/038-fatal-rst-submit-marks-connection-dead.phpt:66-93` はtraceの `wire.frame_out` RST、preface count、deadline後follow-upの `persistent_reused` を見るが、callback fatalは`:52-64` の後にfollow-up callがない。`tests/phpt/039-fatal-submit-request-marks-connection-dead.phpt:57-72` もprefaceとHEADERS/DATA/RST absenceだけを検証する。traceにはsession API invocation event/counterがない。fault seamは実nghttp2をfatal化せずcallをbypassして `NGHTTP2_ERR_NOMEM` を返すため、もしsilent APIが残ってもlibrary crashで自然検出されない。
- Expected model: fatal contractをrequired safety gateにするなら、fault後の同sessionに対する全session API attemptをtest buildでdetectし、`session_del` だけを許す。またcallback fatal後にもfollow-up RPCを実行してdead cache adoptionがないことを確認する。
- Why it matters:今回のdefectはwire output以前のlibrary contract違反であり、silent getter/setter/queryの再導入は通常のintegration assertionでは見えない。将来callback cleanupやdefense-in-depth guardが変わったとき、PHPTがgreenのままfatal sessionを触る退行を許す。
- Recommended fix: test build限定でconnectionにfatal-poison marker/API-attempt counterを持たせるか、nghttp2 call wrapperへassertionを置き、fatal injection後のsame-session API attemptが即test failureになるようにする。PHPT 038にはcallback-policy fatal後のnormal unaryを追加し、fresh preface / `persistent_reused=false` も固定する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: HEAD `2af2d58` のPHPT assertionとtrace event coverageをstatic review。現在のproduction call path自体は `REVIEW-20260711-001` の通りsafeであり、本issueはregression proofの不足に限定する。
- Notes: wire frame absenceとfresh connectionのassertionは有効なので維持し、session API contractのassertionを追加する。

## Review Result

- Blocker: `none`
- High: `1 (Open), 2 (Fixed)`
- Medium: `1 (Open)`
- Low: `1 (Open), 1 (Fixed)`
- Design Decision: `1 (Fixed)`
