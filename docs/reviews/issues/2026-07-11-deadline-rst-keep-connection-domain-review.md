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
- Status: `Open`
- Reviewer role: `HTTP/2/gRPC domain model adversary`
- Finding: `get_persistent_connection` (src/transport.c:1676) は reuse 時に `connection->setup_deadline_abs_us` を採用コールの deadline へ無条件に上書きする。しかし deadline なし (`deadline_abs_us == 0`) の call の write は `send_pending_h2_frames_with_deadline` (src/transport.c:1813) で `current_write_deadline_abs_us = call->deadline_abs_us = 0` となり、`send_callback` / `h2_send_data_callback` (src/transport.c:1792-1797, 1927-1932) が fallback として `setup_deadline_abs_us` を参照する。つまり connection-scoped の 1 個の絶対時刻が、同一 connection 上の複数 stream の write deadline を兼ねている。deadline なしの server streaming call resource が open のまま、同じ channel で短い deadline の unary call が connection を採用 (preflight は `active_stream_count > 0` で early return し reuse を許可: src/transport.c:1031) して DEADLINE_EXCEEDED になると、`setup_deadline_abs_us` は期限切れの unary deadline のまま残る。その後 streaming 側の `next()` が `nghttp2_session_mem_recv` 後の auto WINDOW_UPDATE を `send_pending_h2_frames(connection, call)` で書こうとすると、`h2_connection_send` (src/transport.c:1298-1306) が `remaining_timeout_us < 0` で即 ETIMEDOUT → `NGHTTP2_ERR_CALLBACK_FAILURE` → `mark_connection_dead` となり、無関係な streaming call が UNKNOWN + "HTTP/2 transport deadline exceeded" で死に、温存したはずの connection も破棄される。
- Evidence: `src/transport.c` `get_persistent_connection` / `send_pending_h2_frames_with_deadline` / `send_callback` / `h2_connection_send`、`src/server_streaming_call.c:266,305` (`send_pending_h2_frames(state->call.connection, call)`)
- Expected model: deadline は call (stream) scope の属性であり、connection scope の状態に絶対時刻として保存してはならない。connection-scoped write (SETTINGS/PING/WINDOW_UPDATE 等、call に帰属しない frame) の deadline は「connection setup 中はその setup deadline」「setup 完了後は短い write grace (相対時間) か無期限」のどちらかで、直近に採用した call の deadline を借用するのは scope 違反。今回の refresh は adoption 経路の残留 deadline バグ (issue 記載の潜在バグ) を直したが、同じ根本原因 (絶対時刻の connection-scoped fallback) の並行 stream 変種が残っている。なお本変更前は timeout で connection 自体を破棄していたため in-flight streaming も同時に死んでおり、この変種は「connection を温存する」本変更で初めて意味を持つ経路になった。
- Why it matters: FrankenPHP worker のような長寿命 process で「deadline なし streaming + deadline あり unary の混在」は正当なユースケース。unary の DEADLINE_EXCEEDED が無関係な streaming call を巻き込んで fail させ、かつ本 issue の目的である connection 温存も無効化する。
- Recommended fix: `setup_deadline_abs_us` は connection setup 完了時 (create_h2_connection の return 前) に 0 へクリアし、setup 後の deadline なし write の fallback は `GRPC_LITE_CANCEL_RST_WRITE_GRACE_US` のような「呼び出し時点からの相対 grace」を各 write 開始時に計算して渡す設計へ変更する。そうすれば get_persistent_connection での refresh 自体が不要になる。
- Fix summary: `<pending>`
- Fix commit: `<pending>`
- Verification: deadline なし server streaming resource を open したまま短 deadline unary を timeout させ、その後 streaming next() が継続できることを PHPT で固定する。
- Notes: 逆方向 (deadline なし unary が採用して `setup_deadline_abs_us = 0` になる) は「無期限 write」となり、こちらは意図の範囲内と考えられる。

### REVIEW-20260711-002: timeout した connection を温存した結果、`connection->last_error_detail` / `last_io_errno` の残骸が後続コールの status details に漏れる

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `HTTP/2/gRPC domain model adversary`
- Finding: `connection_recv` は deadline 超過時に `connection->last_io_errno = ETIMEDOUT`、`connection->last_error_detail = "HTTP/2 transport deadline exceeded"` を connection に書き込む (src/transport.c:1459-1461, 1489-1490)。従来はこの直後に connection を破棄していたため残骸は無害だったが、本変更で connection が cache に残るようになった。`grpc_lite_status_details_from_call` (src/transport.c:2227-2229) は call 側の detail が空のとき `connection->last_error_detail` へフォールバックするため、温存された connection 上の後続コールが I/O エラーなしの protocol 起因で失敗した場合 (例: trailers なし stream close → INTERNAL、UNKNOWN fallback 等)、details に前のコールの "HTTP/2 transport deadline exceeded" が誤って載る。bench diagnostic の `connection_last_error_detail` / `connection_last_io_errno` も同様に前コールの値を報告する。
- Evidence: `src/transport.c` `connection_recv` / `grpc_lite_status_details_from_call`、`src/unary_call.c:6-50` (diagnostic 出力)
- Expected model: connection に保存する error 状態は「connection をこれ以上使えなくした事由」に限る。stream-scoped 失敗 (deadline expiry) の記録は call 側 (`call->last_io_error_detail` 等) に閉じるべきで、reuse 可能な connection に残ってはならない。
- Why it matters: status details / diagnostic は障害調査の一次情報。別コールの deadline メッセージが混入すると誤診を誘発する (特に本変更が狙う worker mode の連続 RPC で顕在化しやすい)。
- Recommended fix: `cancel_grpc_call_stream` が RST 送出に成功して connection を温存する場合に `connection->last_error_detail[0] = '\0'` / `last_io_errno = 0` をクリアする。または stream-scoped な read timeout では connection 側フィールドへ書かず call 側だけに記録するよう `connection_recv` の責務を分離する。
- Fix summary: `<pending>`
- Fix commit: `<pending>`
- Verification: timeout 後の同一 connection 上で protocol 失敗を起こし、details に deadline 文言が混入しないことを確認する。
- Notes: `last_ssl_error` にも同種の残留がある。

### REVIEW-20260711-003: `cancel_grpc_call_stream` の `call=NULL` flush は他 stream の pending frame も 50ms grace で巻き込み、失敗時の帰属が失われる

- Severity: `Low`
- Status: `Open`
- Reviewer role: `HTTP/2/gRPC domain model adversary`
- Finding: `send_pending_h2_frames_with_deadline(connection, NULL, now + 50ms)` (src/transport.c:333) は `nghttp2_session_send` で session 内の全 pending frame を flush するため、並行 stream の WINDOW_UPDATE や (flow control で deferred でない) DATA も 50ms grace の下で書かれる。socket buffer が詰まっていて 50ms を超えると `mark_connection_dead` になり、timeout していない並行 call も connection ごと死ぬ。また `call == NULL` のため `current_write_timed_out` → `call->timed_out` / `last_io_errno` の伝播 (src/transport.c:1841-1848) が行われず、巻き込まれた call には connection 側の detail 経由でしか事由が残らない。Decision Log の 50ms 根拠は RST 4+9 bytes の書き込みだけを想定しており、相乗りする frame 量を考慮していない。
- Evidence: `src/transport.c` `cancel_grpc_call_stream` / `send_pending_h2_frames_with_deadline`、`docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md` Decision Log
- Expected model: RST_STREAM 送出は「当該 stream の close を最小コストで通知する」操作であり、失敗時の縮退 (connection 破棄) は従来挙動と同等なので安全側。ただし grace deadline は「RST のみ」の想定値ではなく「その時点の pending frame 一式の flush 上限」であることをモデルとして明示すべき。
- Why it matters: 実害は「温存に失敗して従来どおり接続破棄になる」に留まるが、50ms の設計根拠と実際の書き込み量が食い違っており、将来 grace を詰める変更をした際に並行 stream を不必要に殺す判断ミスを誘発する。
- Recommended fix: `cancel_grpc_call_stream` のコメントおよび issue Decision Log に「grace は RST を含む pending frame 全体の flush 上限で、超過時は従来どおり connection 破棄」と明記する。コード変更は不要。
- Fix summary: `<pending>`
- Fix commit: `<pending>`
- Verification: ドキュメントレビュー。
- Notes: flow control で deferred の DATA は nghttp2 が emit しないため、巻き込みは実質 WINDOW_UPDATE / SETTINGS ACK / 送信可能 DATA に限られる。

### REVIEW-20260711-004: 自分の RST(CANCEL) で mid-message に閉じた stream が truncated-body 判定で `malformed_response_frame = true` になる

- Severity: `Low`
- Status: `Open`
- Reviewer role: `HTTP/2/gRPC domain model adversary`
- Finding: Length-Prefixed-Message の途中で deadline expiry → `cancel_grpc_call_stream` → `on_stream_close_callback` が `stream_closed = true` を立てると、unary の truncated-body 判定 (src/unary_call.c:224-232) と streaming 末尾の同判定 (src/server_streaming_call.c:350-355) は `!call.stream_reset_seen` しか除外条件に持たないため (`stream_reset_seen` は inbound RST 専用: src/transport.c:2163-2165)、local cancel でも `malformed_response_frame = true` が立つ。status code は `grpc_lite_status_code_from_call` (src/status_core.c:12) で `timed_out` が最優先のため DEADLINE_EXCEEDED に保たれ、details も `last_io_error_detail` (= "HTTP/2 transport deadline exceeded") が先に効くので現状のユーザ可視動作は正しい。ただし bench diagnostic の `malformed_response_frame` が偽陽性になり、また将来 taxonomy の優先順位を触った際に INTERNAL へ化ける latent hazard になる。
- Evidence: `src/unary_call.c` build_unary_result、`src/server_streaming_call.c` next core 末尾、`src/status_core.c` `grpc_lite_status_code_from_call`
- Expected model: truncated body が protocol violation (INTERNAL) なのは「server が END_STREAM でメッセージ途中に stream を終えた」場合のみ。client 自身の CANCEL による途中終了は cancellation semantics に属し、malformed とはモデルが異なる。
- Why it matters: 現時点の実害は diagnostic の偽陽性のみだが、status taxonomy の防衛層が `timed_out` の優先順位 1 本に依存している状態であり、レビュー済み taxonomy (PR #28) の意図した層別と一致しない。
- Recommended fix: truncated-body 判定の除外条件に「local cancel で閉じた stream」(例: `call.timed_out` または stream_error_code が自送出 CANCEL 由来であることを示す flag) を加える。
- Fix summary: `<pending>`
- Fix commit: `<pending>`
- Verification: server delay 中 (最初の DATA チャンク受信後) に timeout させ、diagnostic の `malformed_response_frame` が false であることを bench build で確認する。
- Notes: `on_stream_close_callback` は自送出 RST でも `error_code = CANCEL` を渡すため、`stream_error_code == NGHTTP2_CANCEL && timed_out` で判別可能。

### REVIEW-20260711-005: PHPT 033 の 50ms deadline は connection setup / call open を含む予算のため、遅い CI で RST 非送出経路に落ちて flake する

- Severity: `Low`
- Status: `Open`
- Reviewer role: `HTTP/2/gRPC domain model adversary`
- Finding: テストの `['timeout' => 50_000]` (50ms) は TCP connect + SETTINGS 交換 + request 送信を含む deadline になる。負荷の高い CI で setup が 50ms を食い潰すと、(a) unary は `remaining_timeout_us < 0` の early path (src/unary_call.c:120-123, 137-141) で stream を開かずに DEADLINE_EXCEEDED を返し RST_STREAM が送出されない、(b) connection 生成自体が deadline で失敗し cache に connection が残らない、のいずれかになり、`RST_STREAM count == 2` と `persistent_reused === true` の assertion が崩れる。また RST frame の assertion は `stream_id > 0` のみで、timeout した call の stream に対する RST であること (unary と streaming で stream_id が異なること) を固定していない。
- Evidence: `tests/phpt/033-deadline-rst-stream-connection-reuse.phpt`、`src/unary_call.c` early deadline path
- Expected model: 「deadline expiry が read poll 中に発生した場合に RST を送る」という新仕様を pin するテストは、deadline が確実に read poll 中に切れるよう setup 時間に対して十分なマージンを持つべき。
- Why it matters: 本変更の中核仕様を pin するテストが timing flake すると、以後の transport 変更で本仕様の regression 検出力が下がる (flaky 扱いで無視されるリスク)。
- Recommended fix: server delay と timeout の比を保ったまま絶対値を引き上げる (例: unary は server_delay_ms=2000 / timeout=300ms)。あわせて `$rstFrames[0]['stream_id'] !== $rstFrames[1]['stream_id']` 程度の対応付け assertion を追加する。
- Fix summary: `<pending>`
- Fix commit: `<pending>`
- Verification: 修正後 `tools/test/check-phpt.sh` を繰り返し実行して安定性を確認する。
- Notes: streaming 側 (call 3) は確立済み connection を reuse するため flake リスクは主に call 1 に集中する。テストが streaming cancel 経路の修正 (期限切れ call deadline での RST 書き込み失敗 → 接続破棄) を `persistent_reused=true` で間接的に pin できている点は良い。

### REVIEW-20260711-006: SPEC §4.2 の「reuse前のpreflight drainが読み残しを消化する」は保証範囲を過大に記述している

- Severity: `Design Decision`
- Status: `Open`
- Reviewer role: `HTTP/2/gRPC domain model adversary`
- Finding: RST 後の reuse 安全性そのものは検証の結果妥当と判断する: nghttp2 は closed stream 宛の inbound frame を ignore し (closed-stream retention)、auto WINDOW_UPDATE が有効 (NO_AUTO_WINDOW_UPDATE 未設定) なため reset stream 宛 DATA も connection-level flow control window に計上・返却され、HPACK dynamic table も closed stream の HEADERS decode で同期が保たれる。callback 側も `nghttp2_session_get_stream_user_data` が unregister 済みで NULL を返し `call == NULL` guard (src/transport.c:1984, 2015, 2098, 2137) で stale state の漏れはない。ただし SPEC §4.2 の「reuse前のpreflight drainが読み残しを消化する」は正確でない: `preflight_persistent_connection` (src/transport.c:1026-1038) は adoption 時点で socket に bytes が届いている場合しか drain せず、`active_stream_count > 0` なら drain 自体を skip する。遅延して届く reset stream 宛 frame は実際には「次コールの recv loop 内で nghttp2 の closed-stream ignore により消化」されるのが主経路であり、preflight drain は補助にすぎない。
- Evidence: `docs/SPEC.md` §4.2 追記、`src/transport.c` `preflight_persistent_connection` / callbacks の NULL guard
- Expected model: 安全性の根拠は (1) nghttp2 closed-stream ignore + auto window update、(2) callback の stream user_data NULL guard、(3) preflight drain (到着済み bytes のみ) の三層で、主保証は (1)(2)。
- Why it matters: SPEC が preflight drain を主保証として読める記述だと、将来 preflight を最適化 (削除・条件緩和) する際に安全性が壊れると誤判断する、または逆に (1)(2) を壊す変更を preflight があるから安全と誤認する余地がある。
- Recommended fix: SPEC §4.2 の当該文を「キャンセル済みstream宛に届き残ったframeはnghttp2のclosed-stream処理が無視し(flow control計上とHPACK同期は維持される)、adoption時点で到着済みのbytesはpreflight drainが消化する」の順に書き換える。
- Fix summary: `<pending>`
- Fix commit: `<pending>`
- Verification: ドキュメントレビュー。
- Notes: nghttp2 が closed stream を pruning した後に極端に遅い frame が届いた場合は nghttp2 が RST_STREAM(STREAM_CLOSED) 等で応答することがあるが、connection error にはならず reuse 安全性は保たれる。

## Review Result

- Blocker: none
- High: 1
- Medium: 1
- Low: 3
- Design Decision: 1
