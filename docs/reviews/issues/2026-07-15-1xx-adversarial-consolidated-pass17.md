# 1xx informational response adversarial consolidated pass 17 2026-07-15

## Scope

- Commits `20c2dc0` / `6a4902f` / `0e22a8a` / `a80556f` / `6168e2e` / `bf1f324` / `712df8a` / `6470c7f` / `9401067`（current HEAD）
- `src/response_header_phase.[ch]`
- `src/grpc_exchange_state.h`
- `src/transport_core.[ch]`
- `src/status_core.c`
- `src/transport.[ch]`
- `src/diagnostic/bench.c`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `poc/test-server/main.go`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- `tests/unit/test_response_header_phase.c`
- `tests/unit/test_status_core.c`
- `tests/unit/test_transport_core.c`
- 仕様issue、rejected first attempt、pass-1 / pass-3 adversarial review 6 records、pass-2 / pass-4 domain gate、pass-5〜pass-15 consolidated review / gate records、関連design / verification docs

## Reviewer Role

- consolidated adversary（HTTP/2 / gRPC protocol + C safety / lifetime + test / fixture）

## Review Prompt Summary

- pass 17 convergence checkとして、pass-3修正のshared wire-header budget owner、`NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE`と`RESOURCE_EXHAUSTED`のpriority、diagnostic iteration reset、terminal status gate、pushed-stream attribution、PHPT 042 / 043の識別力をcurrent HEADで再監査した。
- 最新increment `9401067`のnormal / invalid regular-before-`:status` classification、shared incomplete-header terminal action、invalid-entry-budget fixture、default-blocking diagnostic fdの実`O_NONBLOCK` oracleを重点的に追跡した。
- issue Decision Logで受容済みの判断は再議論していない。指示どおりtest suite / Dockerは実行せず、runtime確認が必要なfindingにはexact wire probeを記載した。

## Issues

### REVIEW-20260715-001: empty-name invalid regular fieldが`AWAITING_STATUS`のterminal actionを迂回する

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `consolidated adversary（HTTP/2 / gRPC protocol + test / fixture）`
- Finding: pass-15 `REVIEW-20260715-001`の修正は、`:status`より先に届くnormal / invalid regular fieldをshared protocol rejectionへ接続したが、invalid fieldのname lengthが0の場合だけhelperが何も分類せず返る。validな103の後、empty nameを持つEND_STREAM / END_HEADERSなしHEADERSを送りCONTINUATIONを省くと、nghttp2はそのfieldによって後続pseudo-headerを禁止する一方、production / diagnosticはいずれもphaseを`AWAITING_STATUS`、protocol-validのまま残す。このblockは必須`:status`を合法に取得できないことが確定済みなのに、block completionにもshared incomplete-header terminal actionにも到達せず、deadlineなしcall / default-blocking batchがpeerの次の入力を待ち続ける。
- Evidence: `grpc_protocol_reject_response_regular_header_before_status()`は`namelen == 0`を早期return条件に含む（`src/transport.c:2326-2336`）。production invalid callbackはwire budget計上後に同helperを呼び（`src/transport.c:2561-2590`）、diagnosticも同型である（`src/diagnostic/bench.c:525-558`）。nghttp2 v1.69.0の[`nghttp2_http_on_header()`](https://github.com/nghttp2/nghttp2/blob/v1.69.0/lib/nghttp2_http.c#L428-L456)はempty nameをinvalid regular fieldとして`PSEUDO_HEADER_DISALLOWED`へ遷移させ、`NGHTTP2_ERR_IGN_HTTP_HEADER`を返す。[`inflate_header_block()`](https://github.com/nghttp2/nghttp2/blob/v1.69.0/lib/nghttp2_session.c#L3576-L3588)はそのfieldを登録済みinvalid-header callbackへ渡し、callbackが0を返すとfieldをignoreしてheader blockの続きを待つ。HPACK上のexact probeは `HEADERS(:status: 103, END_HEADERS)` → `HEADERS(END_STREAM=1, END_HEADERS=0, BlockFragment=00 00 01 76)`（literal without indexing、empty name、value `v`）→ CONTINUATIONなしでTCPをopenのまま保持するsequenceである。現在のfixtureは非空name `x-after` + NUL-bearing valueだけを使う（`poc/test-server/main.go:924-931,1160-1162`）ため、この早期returnを識別しない。
- Expected model: nghttp2のinvalid-header callbackで`AWAITING_STATUS`中のinvalid regular fieldを受けた場合、empty nameを含めて「後続`:status`を合法に置けない」response-header protocol failureとして扱う。wire budgetを先に適用したうえで`response_header_protocol_error`を確定し、END_HEADERSなしならcaller-selected `RST_STREAM(PROTOCOL_ERROR)`、`response_header_block_incomplete`、production connection terminal quarantine、diagnostic nonblocking finite finishへshared ownerから遷移する。
- Why it matters: hostileまたは壊れたpeerは、4-byteのHPACK fieldとCONTINUATION省略だけでtimeoutなしPHP workerを保持できる。inbound HPACK decoderもblock途中なので、stream-local failureを返さずconnectionを待機・再利用候補のまま残すことは、current designが定めるincomplete-header connection-terminal invariantにも反する。
- Recommended fix: helperのempty-name早期returnを除き、pseudo-header判定を`namelen > 0 && name[0] == ':'`としてzero-length accessを避ける。budget-first orderingは維持する。上記raw controlを追加し、PHPT 042でunary / server streamingのdeadlineなし有限`INTERNAL`、既定details、exact `RST_STREAM(PROTOCOL_ERROR)`、terminal connection、fresh follow-upを、PHPT 043で`failed=1`、`timed_out=false`、`stream_error_code=1`、invalid callback count 1、実fd `O_NONBLOCK`を固定する。
- Fix summary: `grpc_response_header_name_is_regular()`でnon-NULLのempty nameをregular fieldとして分類し、wire budget計上後に既存のregular-before-`:status` rejectionとshared incomplete-header terminal actionへ渡すようにした。exact HPACK `00 00 01 76` controlをraw fixtureへ追加し、production unary / server streamingとdiagnostic batch、pure C predicateの回帰テストを追加した。
- Fix commit: `pending`
- Verification: test-server rebuild / restart PASS。`./tools/test/check-phpt.sh` 29/29 PASS、`./tools/test/check-c-unit.sh` 4/4群 PASS、PHPUnit 31 tests / 116 assertions PASS、production / bench-enabled C static analysis findings none。PHPT 042 / 043でexact empty-name sequenceの`INTERNAL`、default details、exact `RST_STREAM(PROTOCOL_ERROR)`、terminal connection / fresh follow-up、diagnostic failed-not-timedout / callback count 1 / actual fd `O_NONBLOCK`を確認した。HTTP/2 / gRPC domain model reviewはBlocker / High / Medium / Low / Design Decisionすべてnone（`docs/reviews/issues/2026-07-15-1xx-pass17-fix-domain-model.md`）。
- Notes: pass-15 `REVIEW-20260715-001`で修正したfailure modeのempty-name edgeが不十分であることを示す新規findingであり、受容済みdesign decisionの再議論ではない。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `1`
- Low: `none`
- Design Decision: `none`
