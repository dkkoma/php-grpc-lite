# GOAWAY / REFUSED_STREAM transparent retry domain review 2026-07-08

## Scope

- `src/transport.c`
- `src/unary_call.c` / `src/unary_call.h`
- `src/server_streaming_call.c` / `src/server_streaming_call.h`
- `src/wrapper_adapter.c`
- `src/status_core.c` / `src/status_core.h`
- `src/grpc_result.h`
- `src/grpc_exchange_state.h`
- `poc/test-server/main.go`
- `tests/phpt/024-control-semantics.phpt`
- `tests/unit/test_status_core.c`
- relevant docs/test runner changes

## Reviewer Role

- HTTP/2 / gRPC domain model reviewer

## Review Prompt Summary

- GOAWAY / REFUSED_STREAM transparent retryについて、retryable predicate、attempt outcome伝搬、unary/server streamingのretry ownership、GOAWAY/RST cache handling、二段階GOAWAY、deadline、active stream/resource cleanupをレビューした。

## Issues

### Blocker

- none

### High

#### REVIEW-20260708-001: server streaming retryがattemptごとにdeadlineを再作成している

- Severity: High
- Status: Fixed
- Reviewer role: HTTP/2 / gRPC domain model reviewer
- Finding: server streamingのtransparent retryで、gRPC Callのabsolute deadlineではなく、各attemptのopen時に同じrelative timeoutから新しいdeadlineを作っている。`grpc_lite_retry_call_stream()` は retry attempt 1 を開き直すだけで、初回attemptのabsolute deadlineを引き継いでいない。
- Evidence: `src/wrapper_adapter.c:584` が毎回 `grpc_lite_call_timeout_us(call)` を読み、`src/wrapper_adapter.c:626` がrelative `timeout_us`を `server_streaming_call_open_resource()` に渡す。retry pathは `src/wrapper_adapter.c:658`-`src/wrapper_adapter.c:661`。受け側は `src/server_streaming_call.c:54` で `monotonic_us() + timeout_us` を再計算し、そのattempt-local deadlineを `src/server_streaming_call.c:68`、`src/server_streaming_call.c:270`、`src/server_streaming_call.c:288` で使う。unaryは `src/wrapper_adapter.c:489` で一度だけ作った `deadline_abs_us` を `src/wrapper_adapter.c:525` で全attemptへ渡しており、server streamingだけモデルがずれている。issueの完了条件も `docs/issues/open/2026-07-08-goaway-refused-stream-transparent-retry.md:88` でabsolute deadline維持を要求している。
- Expected model: gRPC Callが1つのabsolute deadlineを所有し、各HTTP/2 attemptはその残時間から `grpc-timeout` とpoll/connect/read/write deadlineを導出する。transparent retryは新しいgRPC Callではなく同じCallの再attemptなので、retryで予算を増やしてはいけない。
- Why it matters: 初回attemptがGOAWAY/RST待ちでdeadlineの大半を消費しても、retry attemptに元のtimeoutが丸ごと付与される。ユーザー指定deadlineを超えてOKを返したり、DEADLINE_EXCEEDEDになるべきcallが長くブロックする可能性があり、unaryとserver streamingでdeadline semanticsが分裂する。
- Recommended fix: server streaming open pathもabsolute deadlineを受け取る形にする。例えばwrapper側で初回open前に `deadline_abs_us` を一度だけ計算して `grpc_lite_call_obj` へ保持するか、`grpc_lite_open_call_stream_attempt()` / `server_streaming_call_open_resource()` にabsolute deadlineを渡す。`grpc-timeout` は各attemptでそのabsolute deadlineの残時間から再計算し、retry前に期限切れなら再openせずDEADLINE_EXCEEDEDへ落とす。
- Fix summary: `server_streaming_call_open_resource()` がrelative `timeout_us` ではなくabsolute `deadline_abs_us` を受け取り、wrapper adapterのserver streaming receive pathが1つのabsolute deadlineを計算してretry attempt 1にも同じ値を渡す形に修正された。diagnostic bench call siteもrelative timeoutをabsolute deadlineへ変換してからstream resourceを開くようになった。
- Fix commit: pending
- Verification: 再レビューで `src/wrapper_adapter.c`、`src/server_streaming_call.c`、`src/diagnostic/bench.c` のcall siteを確認。`grpc-timeout`、connect/preflight、read deadlineが同じabsolute deadlineの残時間から導出されている。`tests/phpt/024-control-semantics.phpt` にport 50065のdeadline非延長assertionが追加されている。`git diff --check` 実行済み。
- Notes: retryable predicate、attempt outcome伝搬、GOAWAY/RSTのcache分離、`last_stream_id == INT32_MAX` のactive stream扱いは、読んだ範囲では意図したdomain modelに沿っている。

### Medium

- none

### Low

#### REVIEW-20260708-002: transparent retryの負の不変条件がPHPTで固定されていない

- Severity: Low
- Status: Fixed
- Reviewer role: HTTP/2 / gRPC domain model reviewer
- Finding: 追加PHPTは成功系を確認しているが、「1回だけretry」「userland delivery後はretryしない」「server streaming retryでもdeadlineを延長しない」というdomain invariantを統合境界で固定していない。50061 fixtureはconnection数の奇偶でrefused/OKを切り替えるだけなので、server streaming receive pathが本当に初回refusedからretryしたことも明示的には検証していない。
- Evidence: `tests/phpt/024-control-semantics.phpt:90`-`tests/phpt/024-control-semantics.phpt:102` はGOAWAY transparent retryのOKだけを確認する。`poc/test-server/main.go:821`-`poc/test-server/main.go:827` はport 50061の挙動をprocess-global connection counterに依存させており、unaryとserver streamingが同じfixtureを共有する。`tests/unit/test_status_core.c:94`-`tests/unit/test_status_core.c:110` はpredicate単体のbool条件を確認するが、wrapper/resource lifecycleで「userlandへ返した後に再openしない」ことまでは確認しない。issueの完了条件は `docs/issues/open/2026-07-08-goaway-refused-stream-transparent-retry.md:86`-`docs/issues/open/2026-07-08-goaway-refused-stream-transparent-retry.md:88` に残っている。
- Expected model: transparent retryは「unprocessed attemptを1回だけ」「server streamingでは最初のmessage/statusをuserlandへ渡す前だけ」「absolute deadlineの残時間内だけ」というgRPC Call lifecycle invariantとして、unit predicateだけでなくwrapper/transportを通した統合テストでも固定されるべき。
- Why it matters: retry loopが2回以上走る、message delivery後にstreaming callを再openする、deadlineをretryで延長する、といったdomain modelの破壊が現在のOK-only PHPTをすり抜ける可能性がある。特にserver streamingはretry ownerがreceive pathにあるため、wrapper/resource cleanupを含む統合テストが必要。
- Recommended fix: dedicated fixtureまたはportごとの独立counterで、(1) 2回連続GOAWAY/RST refused後にUNAVAILABLEへ落ちるケース、(2) server streamingで1 message delivery後のGOAWAY/RST/EOFではretryしないケース、(3) retry時に元deadlineを超えたらDEADLINE_EXCEEDEDになるケースを追加する。50061を使い続ける場合も、unary用とserver streaming用のcounterを分け、server streaming testが初回attempt refusedを必ず踏むようにする。
- Fix summary: GOAWAY transparent retry用fixtureがunary用50061とserver streaming用50063に分離され、50064でuserland message delivery後の非retry、50065でretry時のdeadline非延長が追加された。50060のalways refused経路で1回retry後にUNAVAILABLEへ落ちるserver streaming assertionも追加された。fixture docsとpreflight port listは50065まで更新された。
- Fix commit: pending
- Verification: 再レビューで `poc/test-server/main.go`、`tests/phpt/024-control-semantics.phpt`、`docs/verification/test-fixtures.md`、test runner preflight listを確認。server streaming専用counter、always-refused、delivery後非retry、deadline非延長の統合assertionが追加され、元の負の不変条件を固定している。`git diff --check` 実行済み。
- Notes: 実装の責務分離自体は、unaryがperform caller、server streamingがreceive pathという配置になっている。

### Design Decision

- none

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: none
- Design Decision: none
