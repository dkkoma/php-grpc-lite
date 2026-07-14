# GOAWAY / REFUSED_STREAM で拒否されたコールの透過リトライ

- Status: Closed
- Created: 2026-07-08
- Branch: codex/issue-goaway-transparent-retry
- Owner: Claude

## Background

サーバーが GOAWAY を送ると、`last_stream_id` より大きいストリームは「サーバーが一切処理していない」ことが保証される。仕様はこのケースを UNAVAILABLE 扱いとした上で「retry the call elsewhere」とクライアント側のリトライを明示している。

> Clients should consider any stream initiated after the last successfully accepted stream as UNAVAILABLE and retry the call elsewhere.
> — [PROTOCOL-HTTP2.md § GOAWAY Frame](https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-HTTP2.md#connection-management)

現状の実装は `on_frame_recv_callback`（`src/transport.c` の GOAWAY 分岐）で該当ストリームに `stream_refused_seen` を立てて閉じ、`grpc_lite_status_code_from_call`（`src/status_core.c`）で `UNAVAILABLE` をそのままアプリに返す。リトライは行わない。

Google のフロントエンドは max connection age により定期的に GOAWAY を送るため、persistent connection + Spanner という本プロジェクトの主用途では、負荷時に散発的な UNAVAILABLE がアプリ層へ漏れる。

## 公式実装との差異

- **PHP 公式 (ext-grpc = C-core)**: C-core は gRFC A6 の "transparent retry" を実装しており、「RPC がサーバーに一度も到達していない」場合（GOAWAY refused / REFUSED_STREAM を含む）は retry policy の設定に関わらず新しい接続で再送する。
  - 仕様: [gRFC A6 §Transparent Retries](https://github.com/grpc/proposal/blob/master/A6-client-retries.md#transparent-retries) — "RPC never leaves the client" は無制限、"reached the server connection but never seen by server" は 1 回だけ透過リトライ。
  - 実装: [retry_filter.cc](https://github.com/grpc/grpc/blob/master/src/core/client_channel/retry_filter.cc)
- **Go 公式 (grpc-go)**: `handleGoAway` が `LastStreamID` より大きいストリームに `unprocessed` フラグを立て、`csAttempt.shouldRetry` が「初回試行かつ unprocessed」なら透過リトライする。`disableRetry` 設定でも透過リトライ用に送信メッセージをバッファし続ける。
  - [internal/transport/http2_client.go `handleGoAway`](https://github.com/grpc/grpc-go/blob/master/internal/transport/http2_client.go)（`streamID > id && streamID <= upperLimit` → `stream.unprocessed.Store(true)`）
  - [stream.go `csAttempt.shouldRetry`](https://github.com/grpc/grpc-go/blob/master/stream.go)（`if cs.firstAttempt && unprocessed { return true, nil }`）
- **二段階 GOAWAY への注意**: grpc-go はサーバーが「1 回目 GOAWAY (last_stream_id = MaxInt32) → RTT 後に 2 回目 GOAWAY (実際の last_stream_id)」を送るパターンを区別して処理する（[grpc-go#1387](https://github.com/grpc/grpc-go/issues/1387)）。1 回目はリトライトリガーではなく draining トリガーであり、既存ストリームは閉じず新規ストリームだけ止める。2 回目の小さい last_stream_id で初めて `stream_id > last_stream_id` の active stream が retryable refused になる。現行実装の GOAWAY 分岐は 1 回目でも該当ストリームを一律 refused 扱いにするため、この区別を実装する必要がある。

つまり公式実装ではこのケースはアプリに UNAVAILABLE として見えず、php-grpc-lite だけがエラーを露出する。

## Goals

- GOAWAY refused / RST_STREAM(REFUSED_STREAM) で終わり、かつ「サーバー未処理」が保証されるコールを 1 回だけ自動再送する。
- 再送は deadline の残時間内でのみ行う（`grpc-timeout` は残時間で再計算。absolute deadline は初回のものを維持）。

## Non-Goals

- gRFC A6 の retry policy（指数バックオフ、retryableStatusCodes 等）の実装。
- サーバーがすでに処理を開始した可能性のあるコール（データ受信済み等）のリトライ。

## Plan

### retryable 述語（"unprocessed" の厳密化）

`stream_refused_seen && !initial_grpc_status_seen` では不十分（`initial_grpc_status_seen` は initial HEADERS 内の grpc-status しか見ず、metadata / DATA / パーサ途中状態を反映しない）。透過リトライは以下すべてを満たすときのみ:

- `retry_attempt == 0`（初回試行）
- refused である: `stream_refused_seen`（GOAWAY 経路）**または** `stream_reset_seen && stream_error_code == NGHTTP2_REFUSED_STREAM`（RST 経路。現行の `stream_refused_seen` は GOAWAY でしか立たない点に注意: `src/transport.c` の `on_frame_recv_callback` GOAWAY 分岐 / RST_STREAM 分岐、`src/status_core.c` の UNAVAILABLE mapping）
- レスポンス未開始: `http_status` 未受信、response metadata 未受信、`grpc_status` 未受信、`response_message_count == 0`、`response_queue_head == NULL`、レスポンスパーサ途中状態なし（`response_header_len == 0 && response_payload == NULL`）
- server streaming はさらに「userland に 1 件もメッセージ/ステータスを返していない」こと

### attempt outcome の伝搬

`grpc_lite_unary_result` / streaming の next 結果（`src/grpc_result.h`）には status/body/metadata しかなく、リトライ判定に必要な情報が呼び出し側に返らない。`transparent_retryable_unprocessed`、`refused_kind (GOAWAY/RST)`、`response_started` のような outcome フィールドを result に追加する。status code/details 文字列からのリトライ判定はしない。

### リトライのオーナー

- **unary**: `grpc_lite_unary_call_perform_on_connection` の呼び出し側に 1 回限りの再試行ループ。
- **server streaming**: GOAWAY/RST を観測するのは open 時ではなく最初の `next()`（receive path）であることが多い（`server_streaming_call_open_resource` は送信して resource を返すだけ。`src/server_streaming_call.c` / `src/wrapper_adapter.c` の responses 経路）。よってリトライのオーナーは「最初のメッセージ/ステータスを userland に返す前の receive path」とし、`next` 側が retryable_unprocessed を返せるようにして wrapper 層で旧 resource を破棄して再 open する。

### 接続キャッシュの扱い（GOAWAY と RST で分ける）

- GOAWAY refused: 接続は draining。`remove_unusable_persistent_connection` で cache から外し、`get_persistent_connection` で新規接続を取得。
- RST_STREAM(REFUSED_STREAM): 接続自体は healthy のままなので cache から外さない（`remove_unusable_persistent_connection` は usable な接続には何もしない）。同一接続への再 stream で開始する。

### 二段階 GOAWAY

`last_stream_id == INT32_MAX (2147483647)` の 1 回目 GOAWAY では既存ストリームを refused にせず、draining（新規ストリーム停止）のみ。後続 GOAWAY の小さい last_stream_id を受けて初めて `stream_id > last_stream_id` の active stream を retryable refused にする。

### テスト fixture

既存のテストサーバー（50060 ポート系）は接続ごとに常に GOAWAY refused を返すため「リトライ後に成功」を検証できない（`poc/test-server/main.go`、`tests/phpt/024-control-semantics.phpt` は UNAVAILABLE 固定の期待値）。「初回接続は GOAWAY refused、次の接続は成功」という fixture を追加する。

## Progress

- 2026-07-08: `grpc_lite_attempt_outcome` を `grpc_lite_unary_result` / `grpc_lite_streaming_next_result` に追加し、status/details文字列ではなく `transparent_retryable_unprocessed` / refused kind / response started を呼び出し側へ伝搬するようにした。
- 2026-07-08: `status_core.c` にresponse未開始判定とtransparent retryable predicateを追加した。条件は初回attempt、GOAWAY refusedまたは `RST_STREAM(REFUSED_STREAM)`、HTTP status / response metadata / grpc-status / response message / queued payload / parser途中状態なし、かつserver streamingではuserland未delivery。
- 2026-07-08: GOAWAY受信時の `last_stream_id == 2147483647` をdraining-onlyとして扱い、既存active streamをrefused/closedにしないようにした。
- 2026-07-08: unaryはwrapper adapterのperform呼び出し側で1回だけ再attemptするようにした。GOAWAY refusedではdraining connectionをcacheから外して新connectionへ、`RST_STREAM(REFUSED_STREAM)` ではconnectionがusableなら同じconnection上の新streamへ再送する。
- 2026-07-08: server streamingはreceive pathで、最初のmessage/statusをuserlandへ返す前のretryable outcomeだけを1回再openするようにした。stream resource openはabsolute deadlineを受け取り、retry attemptでも同じdeadlineの残時間から `grpc-timeout` とI/O deadlineを導出する。
- 2026-07-08: Go raw h2c fixture ports `50061`-`50065` を追加した。`50061` はunary GOAWAY refused then OK、`50063` はserver streaming GOAWAY refused then OK、`50064` は1 message後GOAWAY、`50065` はGOAWAY refused後にdeadline超過までOKを遅延するfixture。`50062` は `last_stream_id=2147483647` の二段階GOAWAY fixture。
- 2026-07-08: `tests/phpt/024-control-semantics.phpt` を新挙動に更新し、RST/GOAWAY transparent retry成功、always refused後のUNAVAILABLE、delivery後非retry、deadline非延長、二段階GOAWAYを固定した。C unitにはretryable predicate境界条件を追加した。
- 2026-07-08: fixture catalog、code-reading guide、transport design、protocol classification boundary、SPEC、preflight runnerのport listを現行挙動へ更新した。
- 2026-07-08: HTTP/2 / gRPC domain model reviewを実施し、High 1件 / Low 1件を修正後、再レビューで Blocker / High / Medium / Low がすべて none になった。記録: `docs/reviews/issues/2026-07-08-goaway-transparent-retry-domain-review.md`。
- 2026-07-15: PR #26 マージ（merge commit 57e5f93）を確認して Closed。

## Verification

- `git diff --check` : PASS
- `gofmt -w poc/test-server/main.go` : 実行済み
- `./tools/test/check-c-static-analysis.sh` : PASS（ホスト側で実行）
- `./tools/test/check-c-unit.sh` : PASS。protocol_core / status_core / transport_core すべて通過（retryable predicate 境界テスト含む）
- `./tools/test/check-phpt.sh` : PASS 16/16。test-server イメージを新 fixture ポート 50061-50065 込みで再ビルドして実行。透過リトライシナリオを含む 024 も PASS
- `./tools/test/check-crash-ub.sh` : PASS。ASan/UBSan クリーン、fuzz 20000 runs エラーなし
- ビルド修正: `grpc_lite_attempt_outcome_from_call` / `grpc_lite_call_response_started` の宣言が `status_core.h` のみで、拡張本体が参照する `transport.h` に無くコンパイルエラーになっていたため、`transport.h` に宣言を追加（Codex 環境では Docker ビルド不可のため未検出だった）
- Domain model review: `docs/reviews/issues/2026-07-08-goaway-transparent-retry-domain-review.md` で再レビュー後 `Blocker: none / High: none / Medium: none / Low: none`。

## Decision Log

- 2026-07-08: Codex (gpt-5.5) レビューを反映。retryable 述語の厳密化、RST_STREAM(REFUSED_STREAM) の別経路判定、streaming のリトライオーナーを receive path へ、GOAWAY/RST でのキャッシュ処理分離、二段階 GOAWAY、fixture 追加を Plan に追記。
- RST_STREAM(REFUSED_STREAM) 再試行を同一接続で行うか新規接続を強制するかは実装時に決定して記録する（初期方針: 同一 healthy 接続に再 stream）。
- 2026-07-08: 作業branchは `codex/issue-goaway-transparent-retry`。
- 2026-07-08: RST_STREAM(REFUSED_STREAM) のtransparent retryは同一connectionがusableな場合に同じconnection上の新streamで行う。これはstream-local refusalでありconnection drainingではないため、cacheから外さない。
- 2026-07-08: GOAWAY refusedのtransparent retryはdraining connectionをpersistent cacheから外して新connectionで行う。GOAWAYはconnection lifecycle eventであり、新規streamへ使わないため。
- 2026-07-08: retry判定はstatus code/detailsではなくattempt outcomeとしてresultへ伝搬する。status taxonomyはアプリへ返す最終status、attempt outcomeはwrapper orchestrationのretry判断として分離する。
- 2026-07-08: server streaming resource openはrelative timeoutではなくabsolute deadlineを受け取る形にした。transparent retryは同じgRPC Callの再attemptであり、retryでdeadline予算を延長しない。
- 2026-07-08: `GOAWAY(last_stream_id=2147483647)` は二段階GOAWAYの1回目としてdraining-onlyにする。既存streamはrefusedにせず、後続の小さい `last_stream_id` で初めて対象active streamをrefusedにする。

## Close Criteria

- GOAWAY refused のコールがアプリに UNAVAILABLE を返さず、新接続で成功する PHPT がある（unary / server streaming 両方）。
- 2 回連続 refused は 1 回のリトライ後に UNAVAILABLE になる。
- server streaming で 1 件でもメッセージを userland に返した後の GOAWAY/close はリトライしない。
- リトライ時も absolute deadline は初回のものを維持し、超過時は DEADLINE_EXCEEDED になる。
- 二段階 GOAWAY（1 回目 MaxInt32）で既存ストリームが誤って refused/リトライされない。
- 既存の UNAVAILABLE 期待の PHPT（024 等）を新しい挙動に合わせて更新し、全スイートが通る。
