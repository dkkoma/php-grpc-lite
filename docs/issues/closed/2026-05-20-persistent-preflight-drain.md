---
Status: Closed
Owner: Codex
Created: 2026-05-20
Related:
  - docs/issues/open/2026-05-20-pr6-active-ping-redesign.md
---

# persistent preflightでpending TLS dataを破棄せずdrainする

## 目的

persistent HTTP/2 connectionを次RPCへ再利用する直前に、TLS/socket上のpending dataを理由にconnectionを即破棄しないようにする。

HTTP/2ではRPC stream完了後にも `PING` / `PING ACK` / `SETTINGS ACK` / `WINDOW_UPDATE` / `GOAWAY` などのconnection-level frameが届き得る。これらを「未処理データがあるので危険」としてconnection破棄すると、正常なcontrol lifecycleでconnection reuseが壊れる。

## 背景

PR #6検証ブランチで、RPC間にidleがあると `preflight_persistent_connection()` が `SSL_peek() > 0` を理由にconnectionを破棄することを確認した。

main側でも同じpreflight方針が残っているため、active PING実験とは独立したHTTP/2 transport hardeningとして修正する。

## スコープ

- `preflight_persistent_connection()` でpending TLS/socket dataを見つけたら即破棄せず、nonblocking readで短くdrainする。
- 読んだbytesは `nghttp2_session_mem_recv()` に渡してHTTP/2 frameとして処理する。
- ACK等で発生したpending outbound frameは `send_pending_h2_frames(connection, NULL)` でflushする。
- drain後にconnectionがusableなら再利用する。
- `GOAWAY` / EOF / protocol error / socket error などでconnectionがusableでなくなった場合だけ破棄する。

## 非スコープ

- active BDP PINGの採用判断。
- SETTINGS update実験。
- keepalive / BDP estimatorの実装。

## 計画

- [x] preflight drain実装
- [x] local PHPT / C static analysis
- [x] real Spanner marker runでconnection churnが消えるか確認
- [x] issueに検証結果を追記

## 判断ログ

- 2026-05-20: main側で別issueとして扱い、PR #6ブランチはこの修正後にrebaseする方針にした。

## 実装結果

- `preflight_persistent_connection()` はpending TLS/socket dataを見つけても即connectionを破棄しない。
- `drain_pending_connection_data_for_reuse()` で、既に到着済みのbytesをbounded loopで読み、`nghttp2_session_mem_recv()` に渡す。
- `nghttp2_session_want_write()` がtrueになった場合は、現在RPCではなくconnection scopeとして `send_pending_h2_frames_with_deadline(connection, NULL, deadline_abs_us)` でflushする。
- `WANT_READ` / `EAGAIN` まで読み切った場合だけreuseを許可する。
- drain上限到達、nghttp2 parser error、EOF、TLS/socket error、GOAWAYでconnectionがusableでなくなった場合はreuseしない。
- `nghttp2_session_mem_recv()` errorはsocket resetに畳まず、nghttp2 error codeと `nghttp2_strerror()` をconnection detailへ残す。

## 検証

- `./tools/test/check-phpt.sh`: PASS
- `./tools/test/check-c-static-analysis.sh`: PASS
- HTTP/2/gRPC domain review: `docs/reviews/issues/2026-05-20-persistent-preflight-drain-http2-review.md`。Blocker/High/Medium/Low none。
- PHP extension/C safety review: `docs/reviews/issues/2026-05-20-persistent-preflight-drain-c-extension-review.md`。Blocker/High/Medium/Low none。

### real Cloud Spanner marker run

条件:

- target: `vast-falcon-165704 / bench / laravel-bench-db`
- RPC: warmed `ExecuteStreamingSql SELECT 1`
- iterations: 30
- marker script: 各RPC後に `usleep(50000)`
- trace: `GRPC_LITE_TRACE_FILE` + `GRPC_LITE_TRACE_WIRE_BYTES=1`
- log prefix: `var/issue5-bdp-matrix/main-preflight-drain-select1-marker-after-reviewfix`

結果:

| metric | value |
|---|---:|
| elapsed mean | 14.683ms |
| elapsed p50 | 14.614ms |
| elapsed p90 | 17.703ms |
| elapsed p99 | 21.214ms |
| connection preface | 1 |
| unique stream ids | 32 |
| max stream id | 63 |
| preflight reads | 0 |

判断:

- main単体では、50ms idleを入れても同一HTTP/2 connection reuseは維持された。
- 今回のmainにはactive PING実験がないため、PR #6ブランチで見えたactive PING後pending dataのdrain再検証は、PR #6側をmainへrebaseしてから行う。

## 完了条件

- [x] preflight drain実装
- [x] PHPT / C static analysis PASS
- [x] domain review gate PASS
- [x] real Spanner marker runでmainのconnection reuse維持を確認

## 完了ログ

- Status: Closed
- Fix summary: pending TLS/socket dataを即破棄せず、nghttp2へ渡してconnection-level frameを処理し、必要なoutbound frameをflushしてからreuse判定するようにした。
- Fix commit: this commit
