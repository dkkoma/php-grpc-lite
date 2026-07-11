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
- Fix commit: pending
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
- Fix commit: pending
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
- Fix commit: pending
- Verification: PHPT 033、3回連続PASS。
- Notes: 補足で除外されたidle RST候補(MAX_CONCURRENT_STREAMS中のpending request)は対応不要と確認済み。

## Review Result

- Blocker: none
- High: 2 (Fixed)
- Medium: 2 (Fixed)
- Low: 4 (Fixed)
- Design Decision: 1 (Fixed)
- 再レビュー(2026-07-11): 修正コミット caeac40 に対して実施、残指摘 none
- PR #29 敵対的レビュー(2026-07-11): High 1 / Medium 1 / Low 1 を追加受領(REVIEW-20260711-007〜009)、全件Fixed
