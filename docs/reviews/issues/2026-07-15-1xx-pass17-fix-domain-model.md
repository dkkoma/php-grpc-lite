# 1xx informational response pass 17 fix domain model review 2026-07-15

## Scope

- HEAD `9401067` とcurrent working treeのpass-17未コミット差分
- `src/transport.c`
- `src/transport_core.[ch]`
- `src/diagnostic/bench.c`
- `src/status_core.c`
- `src/unary_call.c`
- `src/server_streaming_call.c`
- `poc/test-server/main.go`
- `tests/phpt/042-informational-1xx-adversarial.phpt`
- `tests/phpt/043-informational-1xx-bench-parity.phpt`
- `tests/unit/test_transport_core.c`
- `docs/design/http2-transport-design.md`
- `docs/design/protocol-classification-boundary.md`
- `docs/verification/protocol-model-review-guide.md`
- `docs/verification/test-fixtures.md`
- `docs/issues/open/2026-07-10-informational-1xx-response-handling.md`
- `docs/reviews/issues/2026-07-15-1xx-adversarial-consolidated-pass17.md`

## Reviewer Role

- HTTP/2 / gRPC domain model gate reviewer（pass-17 fix）

## Review Prompt Summary

- `AWAITING_STATUS`中のempty-name invalid regular fieldを、wire budget計上後にregular-before-`:status` protocol failureへ分類できるか確認した。
- production / diagnosticのinvalid-header callbackが同じpredicate、call-local taxonomy、shared incomplete-header terminal actionを通るか確認した。
- END_HEADERS未完了blockのstream RST、connection terminal quarantine、unary / server streaming lifecycle、diagnostic finite finishを確認した。
- exact HPACK fixtureとPHPT / C unitがpass-17修正前のempty-name early returnを識別するか確認した。

## Issues

### Blocker

- none

### High

- none

### Medium

- none

### Low

- none

### Design Decision

- none

## Domain Gate Checks

- Semantic classification / naming: `grpc_response_header_name_is_regular()`はnon-NULLのzero-length nameをregular fieldとして分類し、non-empty pseudo-headerだけを除外する。empty nameを有効なmetadata nameへ昇格させるpredicateではなく、nghttp2のinvalid-header callbackで既にregular-field orderingが確定したfieldをshared rejectionへ接続するためのpure classificationである。`AWAITING_STATUS`、block protocol-valid、regular nameという既存preconditionは維持される。
- Budget-first priority: productionの`on_invalid_header_callback()`とdiagnosticの`bench_on_invalid_header_callback()`はいずれも`grpc_protocol_account_response_header_field()`を先に実行し、超過時は`metadata_too_large`と`RST_STREAM(CANCEL)`を返してname classificationへ進まない。budget内のempty-name fieldだけが`response_header_protocol_error`と`RST_STREAM(PROTOCOL_ERROR)`へ進むため、`RESOURCE_EXHAUSTED`の既存priorityを変更していない。
- Call / stream / connection ownership: malformed responseのpublic taxonomyはcall-local `response_header_protocol_error`が所有し、`grpc_protocol_reject_response_regular_header_before_status()`がprotocol RST codeを選ぶ。END_HEADERS未完了判定、`response_header_block_incomplete`、target RST submit、connectionのclose-after-pending-flush遷移は既存`grpc_protocol_apply_response_header_terminal_action()`へ委譲され、parallelなterminal pathを追加していない。
- Incomplete HPACK lifecycle: regular fieldを観測した時点で後続`:status`は合法に置けないため、block completionを待たずに`response_header_block_protocol_valid=false`、body discard、TEMPORAL callback stopへ遷移する。target `RST_STREAM(PROTOCOL_ERROR)`をbest effortでflushした後、inbound HPACK decoderが未完了のconnectionをdead化し、fresh follow-upへ切り替える既存connection-terminal invariantと一致する。
- Unary / server streaming parity: 両call kindはproduction header callbackとstatus taxonomyを共有する。PHPT 042はempty-name controlを両方へ通し、deadlineなし有限`INTERNAL`、既定details、response count、fresh follow-upを確認する。traceのRST / connection-preface総数は新controlのunary / server streaming両probeがexact `PROTOCOL_ERROR`を送信しconnectionを再利用しないことを識別する。
- Production / diagnostic boundary: raw diagnosticはproductionと同じbudget / name predicate / reject / terminal-action helperを使い、one-shot socket固有のfinite finishだけを`bench_finish_response_header_terminal_action()`でnonblocking化する。PHPT 043は新controlについて`failed=1`、`timed_out=false`、`stream_error_code=PROTOCOL_ERROR`、invalid callback count 1、実fd `O_NONBLOCK`を個別に固定し、diagnostic stateをproduction APIへ漏らしていない。
- Fixture exactness / discriminating power: raw controlはcompleted `:status: 103` blockの後に`HEADERS(END_STREAM=1, END_HEADERS=0, BlockFragment=00 00 01 76)`を送り、CONTINUATIONを送らない。literal without indexingのnew-name length 0 / value `v`をencoderへ正規化させず直接与えるため、pass-17修正前の`namelen == 0` early returnを再現する。C unitはNULL、non-NULL empty、pseudo、regular nameを分離し、zero-length dereference回避とpredicate contractを固定する。
- Error taxonomy: malformed header orderingは`response_header_protocol_error`により`INTERNAL`へ解決され、budget failureの`RESOURCE_EXHAUSTED`、connection cleanup failure、deadlineより先行する既存priorityを維持する。connection quarantineは不完全なconnection-global HPACK stateに起因し、complete malformed blockを一律connection failureへ拡張していない。
- Documentation: pass-17 findingのFix summary / Verification、fixture catalog、仕様issueのProgress / Verificationは、empty-name classification、budget-first ordering、shared terminal action、exact raw fixture、production / diagnostic oracleをcurrent diffと同じscopeで記述する。

## Verification

- current diffとadjacent header phase / status / connection lifecycle codeを静的照合
- production / diagnosticのinvalid-header callbackからwire budget、empty-name predicate、regular-before-status reject、shared incomplete-header terminal actionまでのcall pathを静的照合
- unary / server streamingのclose-after-pending-flush、connection dead化、fresh follow-up lifecycleを静的照合
- raw fixtureのcompleted 103、exact HPACK `00 00 01 76`、END_STREAM / END_HEADERS flags、CONTINUATION省略、peer-observed RST expectationを静的照合
- PHPT 042 / 043のstatus / details、response count、finite finish、exact RST、fresh connection、invalid callback count、実fd `O_NONBLOCK` oracleを静的照合
- C unitのNULL / empty / pseudo / regular name predicate casesを静的照合
- `git diff --check` PASS
- runtime suiteはreview-only laneでは未実行（親agentのrequired verification laneで実行）

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
