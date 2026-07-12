# deadline RST_STREAM(CANCEL) + connection温存 ドメインモデルレビュー 2026-07-11

## Scope

- Branch `codex/issue-deadline-rst-stream-keep-connection` HEAD (3ebfe12)
- `src/transport.c` (`cancel_grpc_call_stream` / `get_persistent_connection` の setup deadline refresh / RST_STREAM trace)
- `src/unary_call.c` (recv loop socket timeout 分岐)
- `src/server_streaming_call.c` (`server_streaming_call_terminate_with_cancel` / timeout cancel 経路)
- `tests/phpt/033-deadline-rst-stream-connection-reuse.phpt`
- `docs/SPEC.md` §4.2 追記

## Reviewer Role

- `HTTP/2/gRPC domain model adversary`

## Review Prompt Summary

- deadline 超過を stream-scoped 失敗として `RST_STREAM(CANCEL)` で処理し persistent connection を温存する変更について、RST 送出後の connection reuse の protocol 安全性 (closed-stream frame / flow control / HPACK)、`setup_deadline_abs_us` refresh の並行 call への影響、`call=NULL` での frame flush の帰属、`on_stream_close_callback` 以降の lifecycle、streaming の cancel/destroy semantics、PHPT 033 のテスト品質を敵対的に検証する。

## Issues

### REVIEW-20260711-001: connection-scoped `setup_deadline_abs_us` を採用コールの deadline で上書きすると、in-flight の deadline なし stream の write が過去の deadline で即死する

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `HTTP/2/gRPC domain model adversary`
- Finding: `get_persistent_connection` (src/transport.c:1676) は reuse 時に `connection->setup_deadline_abs_us` を採用コールの deadline へ無条件に上書きする。しかし deadline なし (`deadline_abs_us == 0`) の call の write は `send_pending_h2_frames_with_deadline` (src/transport.c:1813) で `current_write_deadline_abs_us = call->deadline_abs_us = 0` となり、`send_callback` / `h2_send_data_callback` (src/transport.c:1792-1797, 1927-1932) が fallback として `setup_deadline_abs_us` を参照する。つまり connection-scoped の 1 個の絶対時刻が、同一 connection 上の複数 stream の write deadline を兼ねている。deadline なしの server streaming call resource が open のまま、同じ channel で短い deadline の unary call が connection を採用 (preflight は `active_stream_count > 0` で early return し reuse を許可: src/transport.c:1031) して DEADLINE_EXCEEDED になると、`setup_deadline_abs_us` は期限切れの unary deadline のまま残る。その後 streaming 側の `next()` が `nghttp2_session_mem_recv` 後の auto WINDOW_UPDATE を `send_pending_h2_frames(connection, call)` で書こうとすると、`h2_connection_send` (src/transport.c:1298-1306) が `remaining_timeout_us < 0` で即 ETIMEDOUT → `NGHTTP2_ERR_CALLBACK_FAILURE` → `mark_connection_dead` となり、無関係な streaming call が UNKNOWN + "HTTP/2 transport deadline exceeded" で死に、温存したはずの connection も破棄される。
- Evidence: `src/transport.c` `get_persistent_connection` / `send_pending_h2_frames_with_deadline` / `send_callback` / `h2_connection_send`、`src/server_streaming_call.c:266,305` (`send_pending_h2_frames(state->call.connection, call)`)
- Expected model: deadline は call (stream) scope の属性であり、connection scope の状態に絶対時刻として保存してはならない。connection-scoped write (SETTINGS/PING/WINDOW_UPDATE 等、call に帰属しない frame) の deadline は「connection setup 中はその setup deadline」「setup 完了後は短い write grace (相対時間) か無期限」のどちらかで、直近に採用した call の deadline を借用するのは scope 違反。今回の refresh は adoption 経路の残留 deadline バグ (issue 記載の潜在バグ) を直したが、同じ根本原因 (絶対時刻の connection-scoped fallback) の並行 stream 変種が残っている。なお本変更前は timeout で connection 自体を破棄していたため in-flight streaming も同時に死んでおり、この変種は「connection を温存する」本変更で初めて意味を持つ経路になった。
- Why it matters: FrankenPHP worker のような長寿命 process で「deadline なし streaming + deadline あり unary の混在」は正当なユースケース。unary の DEADLINE_EXCEEDED が無関係な streaming call を巻き込んで fail させ、かつ本 issue の目的である connection 温存も無効化する。
- Recommended fix: `setup_deadline_abs_us` は connection setup 完了時 (create_h2_connection の return 前) に 0 へクリアし、setup 後の deadline なし write の fallback は `GRPC_LITE_CANCEL_RST_WRITE_GRACE_US` のような「呼び出し時点からの相対 grace」を各 write 開始時に計算して渡す設計へ変更する。そうすれば get_persistent_connection での refresh 自体が不要になる。
- Fix summary: `setup_deadline_abs_us` をconnection setup完了時(`create_h2_connection` の成功return直前)に0へクリアし、`get_persistent_connection` でのreuse時上書きを撤回。setup後のdeadlineなしwriteは無期限waitとなり、write deadlineはcall/stream scopeに閉じる。
- Fix commit: caeac40
- Verification: PHPT 033 に並行ケースを追加: deadlineなし server streaming をopenしたまま短deadline unaryをtimeoutさせ、streamingが全messageを受信してOKで完了することを固定。check-phpt.sh 18/18 PASS。
- Notes: 逆方向 (deadline なし unary が採用して `setup_deadline_abs_us = 0` になる) は「無期限 write」となり、こちらは意図の範囲内と考えられる。

### REVIEW-20260711-002: timeout した connection を温存した結果、`connection->last_error_detail` / `last_io_errno` の残骸が後続コールの status details に漏れる

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `HTTP/2/gRPC domain model adversary`
- Finding: `connection_recv` は deadline 超過時に `connection->last_io_errno = ETIMEDOUT`、`connection->last_error_detail = "HTTP/2 transport deadline exceeded"` を connection に書き込む (src/transport.c:1459-1461, 1489-1490)。従来はこの直後に connection を破棄していたため残骸は無害だったが、本変更で connection が cache に残るようになった。`grpc_lite_status_details_from_call` (src/transport.c:2227-2229) は call 側の detail が空のとき `connection->last_error_detail` へフォールバックするため、温存された connection 上の後続コールが I/O エラーなしの protocol 起因で失敗した場合 (例: trailers なし stream close → INTERNAL、UNKNOWN fallback 等)、details に前のコールの "HTTP/2 transport deadline exceeded" が誤って載る。bench diagnostic の `connection_last_error_detail` / `connection_last_io_errno` も同様に前コールの値を報告する。
- Evidence: `src/transport.c` `connection_recv` / `grpc_lite_status_details_from_call`、`src/unary_call.c:6-50` (diagnostic 出力)
- Expected model: connection に保存する error 状態は「connection をこれ以上使えなくした事由」に限る。stream-scoped 失敗 (deadline expiry) の記録は call 側 (`call->last_io_error_detail` 等) に閉じるべきで、reuse 可能な connection に残ってはならない。
- Why it matters: status details / diagnostic は障害調査の一次情報。別コールの deadline メッセージが混入すると誤診を誘発する (特に本変更が狙う worker mode の連続 RPC で顕在化しやすい)。
- Recommended fix: `cancel_grpc_call_stream` が RST 送出に成功して connection を温存する場合に `connection->last_error_detail[0] = '\0'` / `last_io_errno = 0` をクリアする。または stream-scoped な read timeout では connection 側フィールドへ書かず call 側だけに記録するよう `connection_recv` の責務を分離する。
- Fix summary: `cancel_grpc_call_stream` がRST送出に成功して接続を温存する場合に `connection->last_error_detail` / `last_io_errno` / `last_ssl_error` をクリア。
- Fix commit: caeac40
- Verification: check-phpt.sh 18/18 PASS(既存status details系テスト含む)。
- Notes: `last_ssl_error` にも同種の残留がある。

### REVIEW-20260711-003: `cancel_grpc_call_stream` の `call=NULL` flush は他 stream の pending frame も 50ms grace で巻き込み、失敗時の帰属が失われる

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2/gRPC domain model adversary`
- Finding: `send_pending_h2_frames_with_deadline(connection, NULL, now + 50ms)` (src/transport.c:333) は `nghttp2_session_send` で session 内の全 pending frame を flush するため、並行 stream の WINDOW_UPDATE や (flow control で deferred でない) DATA も 50ms grace の下で書かれる。socket buffer が詰まっていて 50ms を超えると `mark_connection_dead` になり、timeout していない並行 call も connection ごと死ぬ。また `call == NULL` のため `current_write_timed_out` → `call->timed_out` / `last_io_errno` の伝播 (src/transport.c:1841-1848) が行われず、巻き込まれた call には connection 側の detail 経由でしか事由が残らない。Decision Log の 50ms 根拠は RST 4+9 bytes の書き込みだけを想定しており、相乗りする frame 量を考慮していない。
- Evidence: `src/transport.c` `cancel_grpc_call_stream` / `send_pending_h2_frames_with_deadline`、`docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md` Decision Log
- Expected model: RST_STREAM 送出は「当該 stream の close を最小コストで通知する」操作であり、失敗時の縮退 (connection 破棄) は従来挙動と同等なので安全側。ただし grace deadline は「RST のみ」の想定値ではなく「その時点の pending frame 一式の flush 上限」であることをモデルとして明示すべき。
- Why it matters: 実害は「温存に失敗して従来どおり接続破棄になる」に留まるが、50ms の設計根拠と実際の書き込み量が食い違っており、将来 grace を詰める変更をした際に並行 stream を不必要に殺す判断ミスを誘発する。
- Recommended fix: `cancel_grpc_call_stream` のコメントおよび issue Decision Log に「grace は RST を含む pending frame 全体の flush 上限で、超過時は従来どおり connection 破棄」と明記する。コード変更は不要。
- Fix summary: `cancel_grpc_call_stream` のコメントとissue Decision Logに「graceはRSTを含むpending frame一式のflush上限で、超過時は従来どおり接続破棄(縮退先は従来挙動で安全側)」と明記。コード変更なし。
- Fix commit: caeac40
- Verification: ドキュメントレビュー。
- Notes: flow control で deferred の DATA は nghttp2 が emit しないため、巻き込みは実質 WINDOW_UPDATE / SETTINGS ACK / 送信可能 DATA に限られる。

### REVIEW-20260711-004: 自分の RST(CANCEL) で mid-message に閉じた stream が truncated-body 判定で `malformed_response_frame = true` になる

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2/gRPC domain model adversary`
- Finding: Length-Prefixed-Message の途中で deadline expiry → `cancel_grpc_call_stream` → `on_stream_close_callback` が `stream_closed = true` を立てると、unary の truncated-body 判定 (src/unary_call.c:224-232) と streaming 末尾の同判定 (src/server_streaming_call.c:350-355) は `!call.stream_reset_seen` しか除外条件に持たないため (`stream_reset_seen` は inbound RST 専用: src/transport.c:2163-2165)、local cancel でも `malformed_response_frame = true` が立つ。status code は `grpc_lite_status_code_from_call` (src/status_core.c:12) で `timed_out` が最優先のため DEADLINE_EXCEEDED に保たれ、details も `last_io_error_detail` (= "HTTP/2 transport deadline exceeded") が先に効くので現状のユーザ可視動作は正しい。ただし bench diagnostic の `malformed_response_frame` が偽陽性になり、また将来 taxonomy の優先順位を触った際に INTERNAL へ化ける latent hazard になる。
- Evidence: `src/unary_call.c` build_unary_result、`src/server_streaming_call.c` next core 末尾、`src/status_core.c` `grpc_lite_status_code_from_call`
- Expected model: truncated body が protocol violation (INTERNAL) なのは「server が END_STREAM でメッセージ途中に stream を終えた」場合のみ。client 自身の CANCEL による途中終了は cancellation semantics に属し、malformed とはモデルが異なる。
- Why it matters: 現時点の実害は diagnostic の偽陽性のみだが、status taxonomy の防衛層が `timed_out` の優先順位 1 本に依存している状態であり、レビュー済み taxonomy (PR #28) の意図した層別と一致しない。
- Recommended fix: truncated-body 判定の除外条件に「local cancel で閉じた stream」(例: `call.timed_out` または stream_error_code が自送出 CANCEL 由来であることを示す flag) を加える。
- Fix summary: `grpc_call` に `locally_cancelled` flagを追加し `cancel_grpc_call_stream` で設定。unary / streaming のtruncated-body判定の除外条件に加えた。
- Fix commit: caeac40
- Verification: check-c-unit.sh / check-phpt.sh PASS。status taxonomyは `timed_out` 優先で従来どおりDEADLINE_EXCEEDED。
- Notes: `on_stream_close_callback` は自送出 RST でも `error_code = CANCEL` を渡すため、`stream_error_code == NGHTTP2_CANCEL && timed_out` で判別可能。

### REVIEW-20260711-005: PHPT 033 の 50ms deadline は connection setup / call open を含む予算のため、遅い CI で RST 非送出経路に落ちて flake する

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `HTTP/2/gRPC domain model adversary`
- Finding: テストの `['timeout' => 50_000]` (50ms) は TCP connect + SETTINGS 交換 + request 送信を含む deadline になる。負荷の高い CI で setup が 50ms を食い潰すと、(a) unary は `remaining_timeout_us < 0` の early path (src/unary_call.c:120-123, 137-141) で stream を開かずに DEADLINE_EXCEEDED を返し RST_STREAM が送出されない、(b) connection 生成自体が deadline で失敗し cache に connection が残らない、のいずれかになり、`RST_STREAM count == 2` と `persistent_reused === true` の assertion が崩れる。また RST frame の assertion は `stream_id > 0` のみで、timeout した call の stream に対する RST であること (unary と streaming で stream_id が異なること) を固定していない。
- Evidence: `tests/phpt/033-deadline-rst-stream-connection-reuse.phpt`、`src/unary_call.c` early deadline path
- Expected model: 「deadline expiry が read poll 中に発生した場合に RST を送る」という新仕様を pin するテストは、deadline が確実に read poll 中に切れるよう setup 時間に対して十分なマージンを持つべき。
- Why it matters: 本変更の中核仕様を pin するテストが timing flake すると、以後の transport 変更で本仕様の regression 検出力が下がる (flaky 扱いで無視されるリスク)。
- Recommended fix: server delay と timeout の比を保ったまま絶対値を引き上げる (例: unary は server_delay_ms=2000 / timeout=300ms)。あわせて `$rstFrames[0]['stream_id'] !== $rstFrames[1]['stream_id']` 程度の対応付け assertion を追加する。
- Fix summary: PHPT 033 のunary/streaming timeoutを300ms、server delayを2000msへ拡大。RST_STREAMのrpc_method帰属とstream_id相違のassertionを追加。
- Fix commit: caeac40
- Verification: 単体で3回連続実行してPASSを確認。
- Notes: streaming 側 (call 3) は確立済み connection を reuse するため flake リスクは主に call 1 に集中する。テストが streaming cancel 経路の修正 (期限切れ call deadline での RST 書き込み失敗 → 接続破棄) を `persistent_reused=true` で間接的に pin できている点は良い。

### REVIEW-20260711-006: SPEC §4.2 の「reuse前のpreflight drainが読み残しを消化する」は保証範囲を過大に記述している

- Severity: `Design Decision`
- Status: `Fixed`
- Reviewer role: `HTTP/2/gRPC domain model adversary`
- Finding: RST 後の reuse 安全性そのものは検証の結果妥当と判断する: nghttp2 は closed stream 宛の inbound frame を ignore し (closed-stream retention)、auto WINDOW_UPDATE が有効 (NO_AUTO_WINDOW_UPDATE 未設定) なため reset stream 宛 DATA も connection-level flow control window に計上・返却され、HPACK dynamic table も closed stream の HEADERS decode で同期が保たれる。callback 側も `nghttp2_session_get_stream_user_data` が unregister 済みで NULL を返し `call == NULL` guard (src/transport.c:1984, 2015, 2098, 2137) で stale state の漏れはない。ただし SPEC §4.2 の「reuse前のpreflight drainが読み残しを消化する」は正確でない: `preflight_persistent_connection` (src/transport.c:1026-1038) は adoption 時点で socket に bytes が届いている場合しか drain せず、`active_stream_count > 0` なら drain 自体を skip する。遅延して届く reset stream 宛 frame は実際には「次コールの recv loop 内で nghttp2 の closed-stream ignore により消化」されるのが主経路であり、preflight drain は補助にすぎない。
- Evidence: `docs/SPEC.md` §4.2 追記、`src/transport.c` `preflight_persistent_connection` / callbacks の NULL guard
- Expected model: 安全性の根拠は (1) nghttp2 closed-stream ignore + auto window update、(2) callback の stream user_data NULL guard、(3) preflight drain (到着済み bytes のみ) の三層で、主保証は (1)(2)。
- Why it matters: SPEC が preflight drain を主保証として読める記述だと、将来 preflight を最適化 (削除・条件緩和) する際に安全性が壊れると誤判断する、または逆に (1)(2) を壊す変更を preflight があるから安全と誤認する余地がある。
- Recommended fix: SPEC §4.2 の当該文を「キャンセル済みstream宛に届き残ったframeはnghttp2のclosed-stream処理が無視し(flow control計上とHPACK同期は維持される)、adoption時点で到着済みのbytesはpreflight drainが消化する」の順に書き換える。
- Fix summary: SPEC §4.2 を「nghttp2のclosed-stream処理が無視(flow control計上とHPACK同期維持) + 到着済みbytesはpreflight drainが消化」の順に書き分け。
- Fix commit: caeac40
- Verification: ドキュメントレビュー。
- Notes: nghttp2 が closed stream を pruning した後に極端に遅い frame が届いた場合は nghttp2 が RST_STREAM(STREAM_CLOSED) 等で応答することがあるが、connection error にはならず reuse 安全性は保たれる。

### REVIEW-20260711-007: RST flush失敗などでdeadになったconnectionを共有する別streamがI/Oを再駆動できる

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 review comment)`
- Finding: `mark_connection_dead` はflagを立てるだけで、`stream_owner_count > 0` ならfd/sessionは残る。server streamingのpullループ(`server_streaming_call_next_resource_core`)は `connection_usable` を確認せずに `send_pending_h2_frames` / `connection_recv` を呼ぶため、next()間に別ownerがconnectionをdeadにした場合(本PRで追加されたRST flush失敗経路を含む)、partial frame後のsessionへwriteを再駆動したり、deadlineなしcallがdead fdで待ち続けたりし得る。
- Evidence: `src/server_streaming_call.c` next core loop、`src/transport.c` `mark_connection_dead` / `send_pending_h2_frames_with_deadline`
- Expected model: dead/fatal sessionは全ownerに対してterminal。以後のsocket/nghttp2 I/Oは禁止。`draining` は一律拒否せず、GOAWAYでadmit済みのstreamは完走できる。
- Why it matters: partial HTTP/2 frame後のsession再駆動はワイヤ破壊、deadlineなしcallのhangは実運用のworker停止に直結する。
- Recommended fix: dead後は全ownerからI/Oを禁止しterminalへ遷移。RST submit・flush失敗をfault injectionで固定。
- Fix summary: `connection_io_allowed()`(dead / fd / sessionを確認し、drainingは許す)を追加し、streaming pullループ先頭でterminal化。`send_pending_h2_frames_with_deadline` にもdead時の早期エラーreturnを追加し、どの経路からもdead sessionを再駆動できないようにした。flush失敗のfault injectionは現行harnessで決定的に再現できないため、fixture `:50066`(1本目のstreamへmessage送出後保持、2本目のrequestでTCP切断)で「別ownerがdeadにした後の生存stream」不変条件を直接固定した。
- Fix commit: 0480479
- Verification: PHPT 034追加(生存streamのnext()がterminalになり、dead後のwire I/Oイベントがトレース上に一切ないこと)。20/20 PASS、対象3テスト3回連続PASS。
- Notes: flush失敗そのもの(partial write)の縮退は従来から `send_pending_h2_frames_with_deadline` 内の `mark_connection_dead` が担保しており、本修正は「dead後の再駆動禁止」を全ownerへ拡張するもの。

### REVIEW-20260711-008: 64KiB backlog時のconnection reuse保証をSPECが過大に記述

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 review comment)`
- Finding: cancel済みstreamのbacklogがpreflight drain上限(64KiB / 64 iterations)を超えて到着済みの場合、drainはEAGAIN境界に達せず `mark_connection_draining` → 新規接続となる。SPEC §4.2の「到着済みbytesはpreflight drainが消化する」は実装の保証範囲を超えていた。
- Evidence: `src/transport.c` `drain_pending_connection_data_for_reuse`(`GRPC_LITE_PREFLIGHT_DRAIN_MAX_BYTES`)、レビュアー実測(message_count=1000 / payload 64KiB → follow-up `persistent_reused=false`)
- Expected model: reuseはbest-effort。cap超過時は安全側(draining → 新規接続)へフォールバックし、後続コールの成功は維持される。
- Why it matters: SPECが実装より強い保証を謳うと将来の変更判断を誤らせる。
- Recommended fix: bounded adoptionの実装、またはbest-effort明記 + cap fallbackテスト。
- Fix summary: 選択肢(b)を採用。SPEC §4.2にreuseがbest-effortであることとcap fallback挙動を明記し、PHPT 035(64KiB×200 messagesのbacklog + user cancel → follow-upがSTATUS_OK、`persistent_reused=false`、connection preface 2本)でfallback安全性を固定。bounded adoption(未読GOAWAY処理後のadmit)は本PRのスコープを超えるため実装しない。
- Fix commit: 0480479
- Verification: PHPT 035追加、3回連続PASS。
- Notes: bounded adoptionを将来実装する場合はPHPT 035のfallback assertionとSPEC §4.2を同時に更新する。

### REVIEW-20260711-009: PHPT 033 part 4のsurvivorがRSTより先にcloseし得る

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 review comment)`
- Finding: survivor(3 messages @100ms、初回遅延なし)は約200msで完了し、300ms deadlineの並行unaryのRSTより先にcloseし得るため、RST時点のmultiplex・RST後のread・deadline scope非漏洩を検証できていない。
- Evidence: `tests/phpt/033-deadline-rst-stream-connection-reuse.phpt` part 4
- Expected model: multiplex検証はRST時点でsurvivorがactiveであることをワイヤ順序で固定する。
- Why it matters: 本PRの中核不変条件(deadlineのstream-scope性)の回帰検出力が下がる。
- Recommended fix: survivor delayをdeadlineより長くし、terminal HEADERSがRST後・survivor宛RSTなし・最終OKをassert。
- Fix summary: survivor delayを700msへ変更(trailersは並行RSTの約1秒後)。トレースからsurvivorのstream idを特定し、(1) survivor宛 `wire.frame_out` RST_STREAMが存在しない、(2) survivorのterminal HEADERS(`flags & END_STREAM`)の `monotonic_us` が並行RSTより後、(3) 全message受信 + STATUS_OK、をassertした。
- Fix commit: 0480479
- Verification: PHPT 033、3回連続PASS。
- Notes: 補足で除外されたidle RST候補(MAX_CONCURRENT_STREAMS中のpending request)は対応不要と確認済み。

### REVIEW-20260711-010: nghttp2 fatal return後にcleanupが `nghttp2_session_set_stream_user_data` を呼ぶ

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 second-pass review comment)`
- Finding: `cancel_grpc_call_stream` で `nghttp2_submit_rst_stream` / `nghttp2_session_send` がfatal(`NGHTTP2_ERR_NOMEM` / `NGHTTP2_ERR_CALLBACK_FAILURE`)を直接返した後も、owner cleanupの `unregister_grpc_call_stream` が `nghttp2_session_set_stream_user_data` を呼ぶ。nghttp2のerror contractはdirect fatal return後に許す操作を `nghttp2_session_del()` のみに限定している。
- Evidence: `src/transport.c` `unregister_grpc_call_stream` / `cancel_grpc_call_stream`
- Expected model: fatal後のsessionはcorrupted。cleanupはlocal bookkeepingのみで、session APIは `nghttp2_session_del()` 以外呼ばない。
- Why it matters: contract違反はcorrupted session上の未定義動作であり、将来のnghttp2更新で顕在化し得る。
- Recommended fix: fatal後のunregisterをlocal bookkeepingのみにし、fault injectionで固定する。
- Fix summary: `unregister_grpc_call_stream` で `connection->dead` の場合は `nghttp2_session_set_stream_user_data` をskipし、local bookkeepingのみにした。fatal経路はすべて `mark_connection_dead` を経由するためdeadゲートで網羅される。stream user dataが残っても、dead接続はsend/recvともに `connection_io_allowed` ゲートで二度と駆動されず、`nghttp2_session_del()` はstream callbackを呼ばないため安全。nghttp2 API呼び出し失敗のfault injectionは現行harnessに注入点がなく決定的に再現できないため、コード上の不変条件(dead ⇒ session APIは `session_del` のみ)をコメントで明文化した。fault injection hookの導入は必要になれば別issueで扱う。
- Fix commit: b13362b
- Verification: PHPT 21/21 PASS。dead後にsession APIへ到達する経路がないことをコードレビューで確認(全fatal経路が `mark_connection_dead` 経由 → unregister skip + I/Oゲート)。
- Notes: PHPT 034(TCP EOF)はこの分岐を通らないが、dead後cleanupの共通経路(unregister skip)は同一。

### REVIEW-20260711-011: draining接続上のadmit済みstreamのcancel/destroyがno-opになりUAFの可能性

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 second-pass review comment)`
- Finding: `cancel_grpc_call_stream` / streaming cancel / destructorが `connection_usable`(draining除外)をgateに使うため、GOAWAY admit済みstreamの明示cancelがno-opになる。resource破棄で `grpc_call` / requestがfreeされるが、nghttp2は `data_provider.source.ptr` を保持したままで、別のadmit済みownerがsessionをsend駆動すると `data_source_read_callback` / `h2_send_data_callback` が解放済みpointerを参照しUAFになる。
- Evidence: `src/transport.c` cancel helper / `src/server_streaming_call.c` cancel_resource・destroy
- Expected model: stream-localなclose gateはsession I/O許可(`connection_io_allowed`)と同じ基準を使い、RST成功またはconnection deadのどちらかを満たしてからcall stateを解放する。
- Why it matters: use-after-freeはメモリ安全性の欠陥で、FrankenPHP worker常駐プロセスでは攻撃面にも運用リスクにもなる。
- Recommended fix: close gateを `connection_io_allowed` に揃える。
- Fix summary: `cancel_grpc_call_stream` / `destroy_server_streaming_call_state` / `server_streaming_call_cancel_resource` のgateを `connection_usable` から `connection_io_allowed` へ変更。draining接続でもRSTをsubmit+flushしてstreamをcloseしてからcall stateを解放する(RST submitでnghttp2がstreamとdata providerを解放)。flush失敗時はdead → 以後sessionは駆動されないため解放済みpointerは参照されない。fixture `:50067`(message + GOAWAY(MaxInt32) + stream保持)を追加し、PHPT 036でdraining接続上のcancelがワイヤにRST_STREAM(CANCEL)を出すことを固定。
- Fix commit: b13362b
- Verification: PHPT 036追加(GOAWAY後のRST送出をトレースで確認、status CANCELLED)。21/21 PASS。
- Notes: unaryのdeadline超過on drainingも同gateで RST が送出されるようになる(従来はskipされ、stack上のcallをdata providerが指したままreturnし得た)。

### REVIEW-20260711-012: shared connection deathのsurvivor statusがUNKNOWNになる

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 second-pass review comment)`
- Finding: dead-terminal guardは `completed=true` にするだけで、`:status 200` と1 message受信済みのsurvivorはstatus fallbackで `UNKNOWN(2)` になる(実測detailsは connection側fallback経由)。gRPC status taxonomyではclient側の「data送信後のconnection break」は `UNAVAILABLE`。PHPT 034の `!== OK` assertは誤分類を見逃す。
- Evidence: `src/server_streaming_call.c` guard branch、`src/status_core.c`、fixture `:50066` 実測
- Expected model: response開始後にclientが観測したconnection breakageは `UNAVAILABLE`。wire status / deadline / cancel / reset のより特異的な信号が先に決まっていればそちらが勝つ。
- Why it matters: statusはretry判断の一次入力。UNKNOWNはUNAVAILABLEと違いgaxの自動retry対象にならないことが多く、worker運用での回復性に直結する。
- Recommended fix: call-ownedにtransport failureをsnapshotし、UNAVAILABLEへ分類。テストはexact assert。
- Fix summary: `grpc_call.connection_broken` flagを追加し、guard branchでconnectionの `last_error_detail` / `last_io_errno` をcallへsnapshotして立てる。`grpc_lite_status_code_from_call` にwire status / deadline / cancel / reset の後・UNKNOWN fallbackの前で `UNAVAILABLE` を返す分岐を追加。PHPT 034はexact `UNAVAILABLE` + details non-emptyをassert。C unit(`test_status_core.c`)に優先順位含む5ケースを追加。
- Fix commit: b13362b
- Verification: C unit `test_connection_broken_mapping` PASS、PHPT 034 PASS。
- Notes: response_startedのままtransparent retry不可(outcome predicateは不変)。

### REVIEW-20260711-013: PHPT 035のbacklogがcancel前にclient側到着済みである保証がない

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 second-pass review comment)`
- Finding: 最初のmessage直後のcancel時点で追加の64KiB超がclient socketへ到着済みとは限らず、実測でPASS 11 / FAIL 1のflake(backlogがcap未満ならreuse成功で `persistent_reused=false` assertが崩れる)。
- Evidence: `tests/phpt/035-preflight-drain-cap-fallback.phpt` 実測
- Expected model: cap fallbackのpinは「backlogがclient kernelに到着済み」をbarrierで証明してから行う。
- Why it matters: 中核仕様のテストがflakeすると回帰検出力が下がる。
- Recommended fix: control connectionでの送信開始指示 + client側到着を証明するbarrier + cap到達のtrace assert。
- Fix summary: fixture `:50068`(data) / `:50069`(control)を追加。"arm"で次のHTTP/2接続をflood対象に指定し、"flood"でSO_SNDBUFを4KiBへ縮小して48KiBのDATAを書き込む。TCPはACKまでデータをsend bufferに保持するため、Write完了="client kernelへの到着"がbarrierになる。調査の結果、default kernel設定(`tcp_rmem` 131072 / `tcp_adv_win_scale=1`)ではclientのTCP receive windowが約64KiBで、production capの64KiB超を未読のままkernelに滞留させることは物理的に不可能(レビュアー実測flakeの根本原因)。そのためdrain capを `grpc_lite.preflight_drain_max_bytes` ini(default 65536、PHP_INI_SYSTEM)へ昇格し、テストは--INI--でcap=16KiB、backlog=48KiB(window内)として決定的にcap超過を作る。cap到達(preflight read >= 16KiB)をトレースでassertしてからfresh connection(`persistent_reused=false`、preface 2本)を期待する。
- Fix commit: b13362b
- Verification: PHPT 035をFAILベースで23回連続実行しflakeなし。PHPT 21/21 PASS(002-iniにdefault値assert追加)。
- Notes: production defaultの挙動は不変。kernel windowの制約はfixtureとPHPTのコメントに記録した。

### REVIEW-20260712-001: nghttp2 fatal returnが全call siteでdead遷移になっていない

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 third-pass review comment)`
- Finding: unary / server streaming の `nghttp2_submit_request` fatal returnはconnectionを再利用可能なまま残し、response callback/parser内の `nghttp2_submit_rst_stream`(14箇所)は戻り値を捨てていた。fatal(NGHTTP2_ERR_NOMEM等)後も同一mem-recv内の後続callbackやsession getter / want_write / session_sendへ進み得る。nghttp2のerror contractはfatal return後に `nghttp2_session_del()` のみを許す。
- Evidence: `src/unary_call.c` / `src/server_streaming_call.c` submit_request、`src/transport.c` parser内RST submit全site
- Expected model: `nghttp2_is_fatal(rv)` を共通判定にし、fatalは即deadへ遷移。callback内fatalは最外のregistered callbackからfailureを返しmem_recvを即時unwind。
- Why it matters: corrupted session上の継続操作は未定義動作。
- Recommended fix: 共通判定 + dead化 + callback unwind + fault injectionで固定。
- Fix summary: `grpc_protocol_submit_rst_stream_in_callback()` を追加し、parser/callback内の全RST submit(14箇所)を置換。fatal時は `mark_connection_dead` + `NGHTTP2_ERR_CALLBACK_FAILURE` を最外callbackへ伝播してmem_recvを即時unwind(`mark_server_streaming_read_ahead_limit_exceeded` はint化して伝播)。unary / streaming の `nghttp2_submit_request` fatalは `mark_connection_dead`。fault injection seam `GRPC_LITE_TEST_FAULT`(env、"rst-submit-fatal" / "submit-request-fatal"、trace hookと同型のprocess単位評価)を導入し、PHPT 038(cancel経路+callback policy経路のfatal → dead → wire上にRSTなし・fresh connection・statusはtimed_out/policy flagが優先)とPHPT 039(submit-request fatal → dead → 再attemptが毎回fresh connection・stream frameがwireに出ない)で固定。
- Fix commit: d54a2ba
- Verification: PHPT 24/24 PASS、対象7テスト15回連続反復でFAILなし。
- Notes: fault seamはfatalを「シミュレート」する(実際のnghttp2呼び出しをskip)ため、fault有効時もsessionは実際にはcorruptしない。テストが固定するのは「fatal後にsession APIへ到達しない」制御フロー。

### REVIEW-20260712-002: same-pullのconnection breakがUNKNOWNのまま

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 third-pass review comment)`
- Finding: `connection_broken` はpull開始時にdead済みのguardでしか立たず、同一pull内のsocket/TLS send failure / non-timeout recv EOF/errorは `completed=true` のみで、response開始済みならUNKNOWN(2)になる(fixture :50066でreviewer実測、details "recv failed: Connection reset by peer")。
- Evidence: `src/server_streaming_call.c` next core loop、`src/unary_call.c` recv loop
- Expected model: client観測のconnection breakはresponse開始後もUNAVAILABLE。nghttp2 direct fatalは別taxonomy。
- Why it matters: UNKNOWNはgax等の自動retry対象にならないことが多く、回復性に直結。
- Recommended fix: connection-I/O由来の各dead transitionでcallへsnapshotし、exact UNAVAILABLEを固定。
- Fix summary: `grpc_call_note_connection_broken()` helper(timed_outはno-op、connectionのerror detail/errnoをcallへsnapshot)を追加し、streaming loopのsend failure / non-timeout recv failure / post-recv send failure、unary loopの同経路すべてに適用。mem_recv(nghttp2)失敗経路は別taxonomyのため適用外。PHPT 034はfixture :50066のcross-pull経路をexact UNAVAILABLEで固定済み(same-pull経路もhelperを共有)。
- Fix commit: d54a2ba
- Verification: PHPT 24/24 PASS(024の50057 mid-stream failure経路含む)、C unit `test_connection_broken_mapping` PASS。
- Notes: unary側の同経路(EOF mid-response)も同時にUNAVAILABLE化した(taxonomy一貫性)。

### REVIEW-20260712-003: preflight drain capが実際のread上限になっていない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 third-pass review comment)`
- Finding: drain loopは合計値で判定するが各readは常に64KiBを要求し、INI=16384でも `requested_len=65536 / result_len=49179` とcapを超えて読む。
- Evidence: `src/transport.c` `drain_pending_connection_data_for_reuse`
- Expected model: capはdrainの実予算であり、単一readがovershootしない。
- Recommended fix: read長を `min(buffer_len, max_bytes - total_read)` にし、合計read ≤ capをassert。
- Fix summary: read長を残余capでクリップ(TLS/socket両経路とtrace requested_lenも更新)。PHPT 035に `preflightBytes <= cap` assertを追加。
- Fix commit: d54a2ba
- Verification: PHPT 035 PASS(sum == 16384を上下両側からassert)。
- Notes: なし

### REVIEW-20260712-004: draining UAFの元shape(pending request DATA + destructor + 別owner drive)が回帰テストで固定されていない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 third-pass review comment)`
- Finding: PHPT 036はGOAWAY後のRST送出のみ固定し、pending DATA provider + destructor + second admitted owner driveのUAF shapeを作らない。`h2_send_data_callback` のlifetimeコメントにも「draining なら session_send しない」という誤った前提が残る。
- Evidence: `tests/phpt/036-draining-connection-cancel-sends-rst.phpt`、`src/transport.c` lifetimeコメント
- Expected model: UAF修正はoriginal shapeのsanitizer regressionで固定する。
- Recommended fix: small initial window + large request A + admitted B + GOAWAY + A destructor + B/WINDOW_UPDATE driveをASan/UBSanで通す。コメント修正。
- Fix summary: fixture `:50070`(INITIAL_WINDOW_SIZE=1024 + connection WINDOW_UPDATE、stream Aへrequest未消費のままmessage応答、BにGOAWAY(MaxInt32)→messageの順で応答、500ms後にAのwindowを開けてBを完走)を追加し、PHPT 037で「A(256KiB request、DATA deferred)を1 message受信後にdraining上でdestruct → RST(A)がGOAWAY後にwireへ出る → 遅延WINDOW_UPDATE(A)を跨いでBがOK完走」を固定。streamingのwire openは最初のrecv batchで遅延実行されるため、Aは1 message読んでstreamをopenしてからdestructする(request DATAはwindow飢餓でdeferredのまま)。`h2_send_data_callback` のlifetimeコメントを「RSTでstream closeしてからfree、またはdead(I/Oゲート)」へ修正。GOAWAYはmessageの前に送出しTCP順序でdraining観測を決定的にした。sanitizer(ASan/UBSan)スイートで全PHPTを実行。
- Fix commit: d54a2ba
- Verification: PHPT 037 PASS、対象7テスト15回反復FAILなし、check-c-sanitizer.sh実行。
- Notes: なし

### REVIEW-20260712-005: test fault seamがproduction buildに組み込まれ、getenv pointerのcacheがrequest跨ぎでdanglingになる

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 fourth-pass review comment)`
- Finding: `grpc_lite_test_fault_enabled()` にbuild guardがなく通常buildのsubmit/cancel/parser pathへ組み込まれる。さらに最初のRPCで取得した `getenv()` のraw pointerをprocess-staticへ保存するため、`putenv()` 後にdangling(reviewer実測: ASan buildの2 request目で `strstr()` がheap-use-after-free)。ZTSでは同期なしstatic read/write raceもある。
- Evidence: `src/transport.c` 旧 `grpc_lite_test_fault_enabled`、`config.m4`
- Expected model: fault seamはdefault-offのtest build defineでcompile out。test buildでもMINIT-owned copy + exact token match(trace hookと同じMINIT copy方式)。
- Why it matters: production pathへのtest seam混入とUAF/data race。
- Recommended fix: compile out + MINIT copy + exact token match + request跨ぎASan/ZTS regression。
- Fix summary: `--enable-grpc-test-fault`(default off)を追加し、seam全体を `PHP_GRPC_LITE_ENABLE_TEST_FAULT` でguard(未定義時は `grpc_lite_test_fault_enabled` をconstant falseマクロにして呼び出しbranchごとcompile out)。有効時はMINITで `GRPC_LITE_TEST_FAULT` を固定バッファへcopyし(getenv pointerを保持しない、MINITはthread起動前なのでZTS読み取り専用で安全)、カンマ区切りのexact token matchで判定。check-phpt / check-zts-phpt / check-c-sanitizer / check-c-coverage のtest buildにflagを追加。PHPT 038に putenv(削除+別値) 後も挙動が変わらないMINIT snapshot regressionと、superstring decoy token("submit-request-fatal-decoy")によるexact match regressionを追加。038/039はseam未buildの場合SKIP(phpinfo行で検出)。
- Fix commit: 3e128d4
- Verification: production build(`--enable-grpc` のみ)がwarningなしでビルドでき038/039がSKIPすること、test-fault build 24/24、ZTSスイート24/24、sanitizerスイート24/24・報告ゼロ。
- Notes: multi-request(FPM)のUAF再現はharness外だが、原因のpointer保持自体を排除しputenv regressionで固定した。

### REVIEW-20260712-006: partial message中のconnection breakがserver streamingでINTERNALに誤分類

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 fourth-pass review comment)`
- Finding: streamingのterminal pathはparser途中stateがあると無条件に `malformed_response_frame` を立て、status priorityで `connection_broken` より先に評価される。fixture :50057(HEADERS + 3-byte partial header + TCP close)でunaryは `UNAVAILABLE(14)`、streamingは `INTERNAL(13)` になる。
- Evidence: `src/server_streaming_call.c` truncated判定、`src/status_core.c` priority
- Expected model: TCP/TLS connection breakは両call kindとも `UNAVAILABLE`。clean END_STREAM途中のframeのみ `INTERNAL`。
- Why it matters: retry判断の一次入力の誤分類。unary/streamingでの非一貫。
- Recommended fix: streamingのtruncated判定に connection break 除外を追加。
- Fix summary: streamingのtruncated判定の除外条件に `!call->connection_broken` を追加しコメントを更新。PHPT 024の :50057 ケースをexact `UNAVAILABLE` に強化し、server streamingケースを追加。
- Fix commit: 3e128d4
- Verification: PHPT 024 PASS(unary/streamingともUNAVAILABLE)。
- Notes: response-started callのtransparent retry不可は不変(outcome predicate非変更)。

### REVIEW-20260712-007: unary submit fatalのdead connectionがcacheに残留し、distinct keyの累積でcache limitに達する

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 fourth-pass review comment)`
- Finding: submit fatal branchはdead化のみでcacheをdetachせず、wrapperもFAILURE即returnするためlazy per-key evictionが働かない。異なるauthority 129件のfatal注入で128 dead entryがcacheを占有し、129件目が "persistent connection cache limit exceeded" になる(reviewer実測)。PHPT 039は同一keyのため検出しない。
- Evidence: `src/unary_call.c` submit fatal branch、`src/transport.c` cache limit
- Expected model: fatal cleanup内でdead entryを即時detachし、最後のowner解放後に破棄。
- Why it matters: 長寿命workerでのcache枯渇 → 新規接続不能。
- Recommended fix: fatal branchで即時detach + 多key regression。
- Fix summary: unary / streaming の submit fatal branch で `detach_persistent_connection_by_ptr()` を呼び即時evict(unaryは `destroy_detached_connection_if_unowned` も追加、streamingはdestroy経路のclear ownerが破棄を担う)。PHPT 039に `grpc.default_authority` を変えた130 keyのsweepを追加し、全件が "nghttp2_submit_request failed"(cache exhaustionでない)かつpreface 133本(dead entryの再利用なし)であることを固定。
- Fix commit: 3e128d4
- Verification: PHPT 039 PASS(130 key sweep含む)、影響8テスト8回反復FAILなし。
- Notes: なし

### REVIEW-20260712-008: unary coreとdiagnostic callerのconnection lifetime契約不一致によるUAF

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 fifth-pass review comment)`
- Finding: submit fatal時、unary coreはconnectionをdead化・cacheからdetachし、owner clear後に破棄してFAILUREを返す。一方 `--enable-grpc-bench` のdiagnostic caller(`grpc_lite_unary`)はFAILURE後も同じraw pointerを `connection_usable()` へ渡す。reviewer実測: bench+test-fault併用ASan buildで `submit-request-fatal` 注入の1コール目からdeterministicにheap-use-after-free(free=`unary_call.c` fatal branch / read=`diagnostic/bench.c` failure branch)。real `nghttp2_submit_request` fatalでも同一branchへ到達する。
- Evidence: `src/diagnostic/bench.c` failure branch、`src/unary_call.c` submit fatal branch
- Expected model: callee/callerでconnection lifetime契約を一意にする。calleeがFAILUREでconnectionを消費するなら、callerはpointerへ再度触れない。
- Why it matters: bench診断buildでのUAF。契約の非一意性はfatal cleanupの変更ごとに同型バグを再生産する。
- Recommended fix: diagnostic failure branchからconnection参照を除去(またはdetach/destroyをkey保持callerへ統一)+ bench+test-fault併用ASan regression。
- Fix summary: 契約を「FAILUREはconnectionを消費して返る(unusable化したbranchはdetach+destroy済み)/ SUCCESSはpointer有効でcallerがevict」に統一してcore定義へ明文化。unary coreはsubmit fatalに加えregister失敗(`mark_grpc_call_stream_registration_failed` はdead化する)・`nghttp2_session_mem_recv` fatalのFAILURE branchでも `detach_persistent_connection_by_ptr` + `destroy_detached_connection_if_unowned` を行い、diagnostic callerのFAILURE branchからconnection参照(`connection_usable` / `remove_unusable_persistent_connection`)を除去。`check-phpt.sh` / `check-c-sanitizer.sh` に `--enable-grpc-bench` を追加し、PHPT 040(`grpc_lite_unary` × `submit-request-fatal` を2 attempt)を追加。旧callerコードを一時復元したASan buildで040が実際にheap-use-after-freeでFAILすることを確認し、修正版でPASSすることを確認(regressionの検出力を実証)。PHPT 001はbench surface露出をMINFO行 "grpc_lite bench diagnostics" とのiff関係でassertする形へ変更(production buildの非露出保証は維持)。
- Fix commit: e424689
- Verification: sanitizerスイート(bench+test-fault)25/25 PASS・報告ゼロ(旧コードでは040がASan UAFでFAIL)、test-fault+benchビルド PHPT 25/25、production build(`--enable-grpc` のみ)warningなし+001 PASS+038/039/040 SKIP、ZTS 24 PASS/1 SKIP(040、bench無効ビルド)。
- Notes: streaming diagnostic caller(`grpc_lite_server_streaming_open`)はraw pointerを保持せず、streaming側のstate destroy経路は従来からunusable時にdetach+destroyするため同型問題なし(確認済み)。

### REVIEW-20260712-009: RST submit fatal時にdeadlineのstatus detailsがconnection errorに上書きされる

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 fifth-pass review comment)`
- Finding: server streamingで1 message受信後・次のpull前にdeadlineが切れ、cancelのRST submitがfatalになると、status codeは `DEADLINE_EXCEEDED(4)` のままだがdetailsが "nghttp2 error: Out of memory" になる(reviewer実測)。code resolverは `timed_out` を最優先する一方、details resolverはconnection error detail(`last_io_error_detail` / `connection->last_error_detail`)をdeadline固有detailsより先に返すため。PHPT 038はcodeしかassertせず見逃していた。
- Evidence: `src/status_core.c` code priority、`src/transport.c` `grpc_lite_status_details_from_call`
- Expected model: deadlineはprimary call outcome、RST送出失敗はsecondaryなconnection-cleanup failure。PHP可視のcode/detailsをともにdeadlineへ揃える。
- Why it matters: リトライ/タイムアウト調整の運用判断がdetails文字列に依存するケースで誤誘導する。code/detailsの不整合はドメインモデル(status taxonomyの優先順位)の破れ。
- Recommended fix: details resolverにtimed_out優先を追加し、PHPT 038へbetween-pull streaming deadlineのexact code/details assertを追加。
- Fix summary: `grpc_lite_status_details_from_call` に「`code == DEADLINE_EXCEEDED && call->timed_out` ならdeadline固有details("HTTP/2 transport deadline exceeded")を返す」分岐をI/O error detail評価より前(server供給の `grpc_message` の直後)へ追加。PHPT 038の既存unary deadlineケースにexact details assertを追加し、between-pull server-streaming deadlineケース(1 message受信 → deadline → fatal RST submit)を新設してexact code/details・受信message数1・後続callのfresh connection(preface 3本)を固定。
- Fix commit: e424689
- Verification: PHPT 038 PASS(unary/streamingともexact details)、影響5テスト(024/033/038/039/040)8回反復FAILなし、C unit 3/3(status taxonomy非変更)、full PHPT 25/25。
- Notes: server供給 `grpc_message` の優先は従来どおり維持(deadline raceでserverがmessage付きtrailersを返した場合の情報は落とさない)。

### REVIEW-20260713-001: production sanitizer laneがbench laneに置換され、production-onlyのmemory bugがgateから外れる

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 sixth-pass review comment)`
- Finding: 第五パスで `check-c-sanitizer.sh` が常に `--enable-grpc-bench` を有効にするよう変更されたが、bench buildは `grpc_call` layoutとunary coreのsignature/分岐を変えるためproductionとは異なるbinary。非benchのcoverage/ZTS laneはsanitizerでなく、CIのCrash/UB laneはprotocol-core fuzzerのみ。production-onlyのlayout/分岐に入るmemory bugがsanitizer gateから外れる。
- Evidence: `tools/test/check-c-sanitizer.sh`、`src/grpc_exchange_state.h` のbench限定member、`src/unary_call.c` のbench限定引数/分岐
- Expected model: production full sanitizerとbench+fault sanitizerの2 laneを実行し、どちらのbinaryもsanitizer gateに残す。
- Why it matters: sanitizerは最後のmemory-safety gate。検証対象binaryの置換はgapを静かに作る。
- Recommended fix: 2 lane実行。
- Fix summary: `check-c-sanitizer.sh` のPHPT実行を `run_phpt_lane()` に共通化し、**production lane**(`--enable-grpc` のみ。純production binaryで全PHPTを実行、fault/bench系4テストはSKIP)と **bench+fault lane**(`--enable-grpc-test-fault --enable-grpc-bench`。全26テスト実行)を順に実行する構成へ変更。各laneでbuild→extension load確認→run-testsを行い、laneごとにログを分離。
- Fix commit: 24abba3
- Verification: sanitizerスイート実行で production lane 22 PASS / 4 SKIP、bench-fault lane 26/26 PASS、ASan/UBSan報告ゼロ(halt_on_error下でexit 0)。
- Notes: 旧来のlaneは `--enable-grpc --enable-grpc-test-fault` だったため、fault seamすら持たない純productionのsanitizer実行はむしろ従来より厳密になった。

### REVIEW-20260713-002: PHPT 001のbench期待値が同一moduleのMINFO由来で外部不変条件にならない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 sixth-pass review comment)`
- Finding: 001はMINFOの "grpc_lite bench diagnostics" 行からbench関数の露出期待値を導くため、MINFOと関数表の整合性しか検査できない。build flagの誤りで両方が有効になった場合(production laneへのbench混入)もPASSする。
- Evidence: `tests/phpt/001-load.phpt`
- Expected model: 期待値はmodule外部(runner)が宣言し、production lane自身が非露出をassertする。
- Recommended fix: `GRPC_LITE_EXPECT_BENCH=0|1` をrunnerから渡す。
- Fix summary: 001の期待値を `GRPC_LITE_EXPECT_BENCH` 環境変数(未設定=production期待、つまり非露出)へ変更し、MINFO行と関数露出の両方を同じ外部期待と照合。benchをbuildするrunner(`check-phpt.sh`、sanitizer bench-fault lane)だけが `=1` を宣言し、ZTS/coverage/素のproduction実行はデフォルトの非露出assertを受ける。
- Fix commit: 24abba3
- Verification: bench lane(EXPECT=1)26/26、sanitizer production lane(EXPECT=0)で001 PASS、ZTS(bench無効・EXPECT未設定)24 PASS / 2 SKIP。
- Notes: 未設定デフォルトを非露出側に倒したため、bench buildを期待宣言なしで走らせると001がFAILする(意図的に露出を宣言させる設計)。

### REVIEW-20260713-003: unary mem-recv fatalのownership修正がsanitizer regressionで固定されていない

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `PR adversary (PR #29 sixth-pass review comment)`
- Finding: e424689はsubmit fatalに加えregister失敗と `nghttp2_session_mem_recv` fatalのownership cleanupも変更したが、PHPT 040は `submit-request-fatal` しか通らない。reviewerはfocused ASanで現行コードの安全性は確認済みだが、repository regressionが存在しない。
- Evidence: `src/unary_call.c` mem_recv fatal branch、`tests/phpt/040`
- Expected model: 変更したFAILURE branchはそれぞれregressionで固定する。
- Recommended fix: `rst-submit-fatal` × 小receive limit + 大responseでmem-recv fatalへ到達するPHPT(2 attempt・ASan無報告・cache非残留)。
- Fix summary: PHPT 041を追加。`GRPC_LITE_TEST_FAULT=rst-submit-fatal` + `max_receive_message_length=8` + 1KiB responseで、callback内policy RSTのfatal → `nghttp2_session_mem_recv` fatal → unary coreのdetach+destroy → diagnostic callerがpointerへ触れない経路を2 attemptで固定。さらに130 distinct-key sweep(authority可変)で全attemptが "nghttp2_session_mem_recv failed"(cache exhaustionでない)かつpreface 132本(消費済みconnectionの再利用なし)であることをassert。mem_recv branchの `detach_persistent_connection_by_ptr` を一時除去した状態で041が実際にFAILする(sweepがcache limitに到達する)ことを確認し、regressionの検出力を実証した。041はsanitizer bench-fault laneでも実行される。
- Fix commit: 24abba3
- Verification: PHPT 041 PASS(NTS/sanitizer bench-fault lane)、detach除去時FAIL確認、影響4テスト(001/038/040/041)8回反復FAILなし。
- Notes: register失敗branchは `nghttp2_session_set_stream_user_data` の失敗を外部から誘発できないためPHPT固定対象外(レビュー指摘のスコープもmem-recvのみ)。

## Review Result

- Blocker: none
- High: 7 (Fixed)
- Medium: 9 (Fixed)
- Low: 8 (Fixed)
- Design Decision: 1 (Fixed)
- 再レビュー(2026-07-11): 修正コミット caeac40 に対して実施、残指摘 none
- PR #29 敵対的レビュー(2026-07-11): High 1 / Medium 1 / Low 1 を追加受領(REVIEW-20260711-007〜009)、全件Fixed
- PR #29 敵対的レビュー第二パス(2026-07-11、HEAD 6795a5a): High 2 / Medium 2 を追加受領(REVIEW-20260711-010〜013)、全件Fixed
- PR #29 敵対的レビュー第三パス(2026-07-12、HEAD be1b97e): High 1 / Medium 1 / Low 2 を追加受領(REVIEW-20260712-001〜004)、全件Fixed
- PR #29 敵対的レビュー第四パス(2026-07-12、HEAD 2af2d58): High 1 / Medium 2 を追加受領(REVIEW-20260712-005〜007)、全件Fixed
- PR #29 敵対的レビュー第五パス(2026-07-12、HEAD 287bc93): High 1 / Medium 1 を追加受領(REVIEW-20260712-008〜009)、全件Fixed
- PR #29 敵対的レビュー第六パス(2026-07-13、HEAD 3081608): Medium 1 / Low 2 を追加受領(REVIEW-20260713-001〜003)、全件Fixed
