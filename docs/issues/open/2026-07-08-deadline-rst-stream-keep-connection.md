# deadline 超過時に RST_STREAM(CANCEL) を送って接続を温存する

- Status: Open
- Created: 2026-07-08
- Branch: codex/issue-deadline-rst-stream-keep-connection
- Owner: Claude

## Background

gRPC のキャンセル（deadline 超過を含む）はストリーム単位の `RST_STREAM` で行い、接続は生かしたまま他のコール／後続のコールで再利用するのが仕様の想定。接続全体を落とすのは接続レベル障害のときのみ。

> RST_STREAM error codes → CANCEL(8): Mapped to call cancellation when sent by a client.
> — [PROTOCOL-HTTP2.md § Errors](https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-HTTP2.md#errors)

現状の実装は unary の受信ループ（`src/unary_call.c` の `grpc_lite_unary_call_perform_core_on_connection`、`nread <= 0` 分岐）で、socket timeout を検出すると `call.timed_out = true` にした後 `mark_connection_dead(connection, errno)` で**接続ごと破棄**する。RST_STREAM は送らない。

persistent connection が前提の FrankenPHP worker 用途では、1 回の DEADLINE_EXCEEDED のたびに TCP + TLS ハンドシェイクからやり直しになり、レイテンシ面の実害がある。また RST_STREAM を送らないため、サーバー側は クライアントが消えるまで処理を継続し得る。

## 公式実装との差異

- **PHP 公式 (ext-grpc = C-core)**: deadline 超過はコールのキャンセルとして扱われ、chttp2 トランスポートが該当ストリームに `RST_STREAM(CANCEL)` を送出する。接続（channel/transport）は維持され、後続コールで再利用される。
  - 実装: [chttp2_transport.cc `grpc_chttp2_cancel_stream`](https://github.com/grpc/grpc/blob/master/src/core/ext/transport/chttp2/transport/chttp2_transport.cc)
- **Go 公式 (grpc-go)**: context の期限切れ／キャンセルで `ClientStream.Close(err)` が呼ばれ、`err != nil` のとき `rstCode = http2.ErrCodeCancel` で RST_STREAM を送る。トランスポート（接続）はそのまま。
  - 実装: [internal/transport/client_stream.go `ClientStream.Close`](https://github.com/grpc/grpc-go/blob/master/internal/transport/client_stream.go)（`rstCode = http2.ErrCodeCancel`）

なお本実装の streaming 側にはユーザー起点キャンセル用の `cancel_active_server_streaming_call_state`（`src/transport.c`）が既に RST_STREAM(CANCEL) を実装済みで、deadline 経路だけが接続破棄になっている。

## Goals

- unary / server streaming の deadline 超過時に、接続を殺す代わりに `nghttp2_submit_rst_stream(NGHTTP2_CANCEL)` を送出して当該ストリームのみ閉じる。
- RST 送出後、接続を persistent cache に残して次コールで再利用できるようにする。

## Non-Goals

- deadline 検出精度そのものの変更（poll ベースの現行方式は維持）。
- 送信途中（DATA が書きかけ）で write がブロックしているケースの救済。write ブロック中のタイムアウトは HTTP/2 フレーム境界を保証できないため、従来どおり接続破棄でよい。

## Plan

- unary 受信ループの socket timeout 分岐で、`mark_connection_dead` の代わりに RST_STREAM(CANCEL) を submit → `send_pending_h2_frames` → 短時間（数十 ms 上限）の drain で stream close を待つ。
- drain 中に close が確認できなければ従来どおり接続破棄にフォールバック（読み残しバイトを持つ接続を再利用しないため）。
- streaming の read timeout 経路にも同じ処理を適用。
- PHPT: 遅延応答サーバーに対して timeout → 同じ persistent connection で後続コールが成功（`persistent_reused = true`）することを確認。

## Progress

- 2026-07-11: `cancel_grpc_call_stream()`(`src/transport.c`)を追加。stream単位の RST_STREAM(CANCEL) submit + 送出を共通化し、unary の socket timeout 分岐(`src/unary_call.c`)を `mark_connection_dead` から本helperへ置き換えた。
- 2026-07-11: 既存の streaming cancel 経路の潜在バグを修正。`cancel_active_server_streaming_call_state` / `server_streaming_call_terminate_with_cancel` は `send_pending_h2_frames(connection, &state->call)` で **期限切れの call deadline** をwrite deadlineに使っており、deadline経路ではRSTが書けず即 `mark_connection_dead` になっていた。helperはRST書き込みに専用のgrace deadline(`GRPC_LITE_CANCEL_RST_WRITE_GRACE_US` = 50ms)を使う。
- 2026-07-11: `setup_deadline_abs_us` をconnection setup完了時(`create_h2_connection` の成功return直前)に0へクリアするようにした。従来は接続作成時のcall deadlineがconnection-scopedなwrite fallback deadlineとして残留し、deadline超過後に温存した接続でdeadlineなしの後続コールが即 ETIMEDOUT になった(timeout時に接続を破棄していた従来挙動では露見しなかった潜在バグ)。当初はreuse時に採用コールのdeadlineで上書きする形で修正したが、ドメインモデルレビュー REVIEW-20260711-001 の指摘(並行するdeadlineなしstreamのwriteが他コールの期限切れdeadlineを借用して即死するscope違反)を受け、setup完了時クリアへ変更した。
- 2026-07-11: トレースの `wire.frame_out` に RST_STREAM の `error_code` を追加(inbound側と対称)。
- 2026-07-11: PHPT `tests/phpt/033-deadline-rst-stream-connection-reuse.phpt` を追加。unary / server streaming それぞれの deadline 超過後に、ワイヤ上の RST_STREAM(error_code=8、timeoutしたcallのstreamに帰属) と、後続 unary コールの `persistent_reused=true` + STATUS_OK をトレースで固定。
- 2026-07-11: ドメインモデルレビュー([2026-07-11-deadline-rst-keep-connection-domain-review](../../reviews/issues/2026-07-11-deadline-rst-keep-connection-domain-review.md)、High 1 / Medium 1 / Low 3 / Design Decision 1)の指摘を全件修正。`setup_deadline_abs_us` のsetup完了時クリア(001)、RST送出成功時のconnection-scoped error残骸クリア(002)、grace deadlineの意味の明文化(003)、`locally_cancelled` flagによるtruncated-body誤判定の除外(004)、PHPT 033のタイミングマージン拡大とRST帰属assertion追加(005)、SPEC §4.2のreuse安全性根拠の書き分け(006)。

## Verification

- `tools/test/check-phpt.sh`: 18/18 PASS(新規 033 を含む)。
- `tools/test/check-c-unit.sh`: protocol_core / status_core / transport_core すべてPASS。
- PHPUnit 統合テスト: 31 tests / 116 assertions OK。
- `tools/test/check-c-static-analysis.sh`: pass(指摘なし)。
- トレース実測: timeout時に `wire.frame_out` RST_STREAM error_code=8 が出て、直後のコールが `persistent_reused=true` で成功することを確認(2026-07-11)。
- ドメインモデルレビュー再レビュー(修正コミット caeac40 対象): 全6件adequate、新規指摘なし。PHPT 033単体3回 + スイート3回連続PASSで安定性確認(2026-07-11)。
- PR #29 敵対的レビュー対応(REVIEW-20260711-007〜009): PHPT 20/20 PASS(新規034/035含む)、対象3テスト3回連続PASS、C unit / PHPUnit 31 tests / 静的解析 pass(2026-07-11)。
- PR #29 敵対的レビュー第二パス対応(REVIEW-20260711-010〜013): PHPT 21/21 PASS(新規036、002-ini更新)、対象4テストをFAILベースで23回連続実行しflakeなし、C unit(connection_broken taxonomy追加) / PHPUnit / 静的解析 pass(2026-07-11)。
- PR #29 敵対的レビュー第三パス対応(REVIEW-20260712-001〜004): PHPT 24/24 PASS(新規037/038/039)、対象7テスト15回反復FAILなし、sanitizer(ASan/UBSan)スイート24/24 PASS・報告ゼロ、C unit / PHPUnit / 静的解析 pass(2026-07-12)。
- PR #29 敵対的レビュー第四パス対応(REVIEW-20260712-005〜007): test-fault build 24/24 PASS、production build(seamなし)クリーンビルド+038/039 SKIP、ZTSスイート24/24、sanitizerスイート24/24・報告ゼロ、影響8テスト8回反復FAILなし、C unit / PHPUnit 31 / 静的解析 pass(2026-07-12)。
- PR #29 敵対的レビュー第五パス対応(REVIEW-20260712-008〜009): test-fault+benchビルド PHPT 25/25 PASS(新規040、038強化)、sanitizerスイート(bench併用)25/25・報告ゼロ — 旧callerコードを一時復元すると040がASan heap-use-after-freeで実際にFAILすることを確認しregressionの検出力を実証、production build warningなし+001 PASS+fault系SKIP、ZTS 24 PASS/1 SKIP(040)、影響5テスト8回反復FAILなし、C unit 3/3 / PHPUnit 31 / cppcheck(production+bench両構成)pass(2026-07-12)。

## Decision Log

- 計画にあった「RST送出後の短時間drain」は実装しない(2026-07-11)。RST_STREAM submit時点でnghttp2はstreamをclose済み扱いにするためdrainで待つ対象がなく、読み残しframeはnghttp2のclosed-stream無視 + 次回reuse時のpreflight drain(`preflight_persistent_connection`)が既に消化する。streaming側のuser cancel経路も同じ前提で運用済み。
- RST書き込みのgrace deadlineは50msとする。このgraceはRST 13 bytesだけでなく、その時点のsession pending frame一式(並行streamのWINDOW_UPDATE / SETTINGS ack / 送信可能DATAを含む)のflush上限である。localhost/同リージョンのRTTに対して十分で、flushがそれ以上blockする接続は再利用に値しないため従来どおり`mark_connection_dead`にフォールバックする(縮退先は従来挙動の接続破棄で安全側)。
- Non-Goalどおり、write block中(`send_callback`内)のタイムアウトは接続破棄のまま維持する。frame境界を保証できないため。
- PR #29 敵対的レビュー第五パス(High: unary coreとbench diagnostic callerのconnection lifetime契約不一致によるUAF / Medium: RST submit fatal時にdeadline detailsがconnection errorに上書きされる)を受けて追加対応(2026-07-12)。unary coreのFAILURE契約を「connectionを消費して返る」に統一(submit fatalに加えregister失敗・mem_recv fatalでもdetach+destroy、diagnostic callerはFAILURE後にpointerへ触れない)し、details resolverにtimed_out優先を追加してcode/detailsをともにdeadlineへ整合。check-phpt / check-c-sanitizer に `--enable-grpc-bench` を追加し、PHPT 040(bench×fault UAF regression)とPHPT 038のbetween-pull streaming deadline(exact code/details)で固定。
- PR #29 敵対的レビュー第四パス(High: fault seamのproduction混入とgetenv pointer dangling / Medium: streaming partial-message connection breakのINTERNAL誤分類 / Medium: submit fatalのdead entry cache残留)を受けて追加対応(2026-07-12)。fault seamは `--enable-grpc-test-fault`(default off)でcompile outし、test buildではMINIT-owned copy + exact token matchに変更。streamingのtruncated判定に `connection_broken` 除外を追加(:50057で両call kindがUNAVAILABLE)。submit fatal branchで `detach_persistent_connection_by_ptr` による即時evictを追加(130 distinct key sweepで固定)。
- PR #29 敵対的レビュー第三パス(High: fatal全call siteのdead遷移 / Medium: same-pull connection breakのUNAVAILABLE化 / Low: drain capのread上限化 / Low: UAF original shapeのsanitizer固定)を受けて追加対応(2026-07-12)。parser/callback内の全RST submitを `grpc_protocol_submit_rst_stream_in_callback()` 経由にしてfatal→dead+mem_recv即時unwind、`grpc_call_note_connection_broken()` をsame-pullの全connection-I/O失敗経路へ適用、fault injection seam `GRPC_LITE_TEST_FAULT` を導入(PHPT 038/039)、fixture :50070 + PHPT 037でUAF original shapeを固定しsanitizerスイートで検証。
- PR #29 敵対的レビュー第二パス(High: nghttp2 fatal後のsession API / High: draining上cancelのno-op化によるUAF / Medium: survivor statusのUNKNOWN / Medium: PHPT 035のbarrier欠如)を受けて追加対応(2026-07-11)。dead後のcleanupはlocal bookkeepingのみ(unregisterのdead skip)、stream-scoped closeのgateを `connection_io_allowed` に統一(drainingでもRST送出)、shared connection deathは `connection_broken` flag経由で `UNAVAILABLE` に分類。PHPT 035はkernel TCP receive window(~64KiB)の物理制約によりproduction cap(64KiB)超のbacklog滞留が不可能と判明したため、`grpc_lite.preflight_drain_max_bytes` iniを導入し、SO_SNDBUF縮小によるACK barrier(fixture :50068/:50069)+小cap指定で決定的化した。
- PR #29 敵対的レビュー(High: dead session再駆動 / Medium: preflight drain cap時のreuse保証 / Low: multiplexテストの順序)を受けて追加対応(2026-07-11)。dead connectionは全owner streamに対してterminalとし(`connection_io_allowed` + streaming pullループのguard + send経路のdead早期return、drainingはadmit済みstream継続を許可)、reuseはbest-effortとSPECに明記してcap fallbackをPHPT 035で固定、PHPT 033のsurvivorはRST後まで生存させワイヤ順序をassertする。fault injectionによるRST flush失敗の決定的再現は現行harnessでは不可のため、fixture :50066のTCP切断で同じ不変条件(dead-under-another-owner)を固定した。

## Close Criteria

- deadline 超過後に同一 persistent connection が再利用される PHPT が通る。
- timeout 時にワイヤ上へ RST_STREAM(CANCEL) が出ることをトレース（`GRPC_LITE_TRACE_FILE`）で確認。
