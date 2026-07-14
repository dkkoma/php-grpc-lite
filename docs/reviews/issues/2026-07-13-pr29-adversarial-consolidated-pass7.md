# PR #29 adversarial re-review 統合版 2026-07-13（第七パス）

## Scope

- PR #29: base `e49d4be` / HEAD `199bf01`
- 今回の対応commit: `24abba3`（前回reviewed HEAD `3081608`からの修正）
- 元issue: `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md`
- 統合元:
  - `docs/reviews/issues/2026-07-13-pr29-adversarial-domain-pass7.md`
  - `docs/reviews/issues/2026-07-13-pr29-adversarial-lifetime-pass7.md`
  - `docs/reviews/issues/2026-07-13-pr29-adversarial-sanitizer-pass7.md`

## Reviewer Role

- adversarial re-review consolidator / GitHub delivery editor

## Review Result

- Decision: `REQUEST_CHANGES` 相当
- Open: `Low 3`
- 前回Medium（production sanitizer laneのbench置換）は2 lane化で修正済み。
- 前回Lowのうち、bench surfaceの期待値外部化は修正済み。mem-recv fatal regressionは到達性、caller UAF、cache detach / non-reuseを固定したが、terminal destroyのoracleだけ不足する。
- 現HEADのproduction codeで新たなlifetime defectは確認していない。残件はproduction/test boundary、resource release regression、issue current-state documentationの検証穴である。

## Open Findings

### REVIEW-20260713-004: production sanitizer laneがtest-fault seam非露出を外部oracleで固定しない

- Severity: `Low`
- Status: `Open`
- Reviewer role: production / diagnostic boundary adversary
- Finding: PHPT 001はbench surfaceをrunner由来の`GRPC_LITE_EXPECT_BENCH`へ外部化したが、同じproduction boundaryのtest-fault seamには外部期待がない。production laneへ誤って`--enable-grpc-test-fault`を追加しても、038/039はmodule自身のMINFOを見てSKIPを解除してPASSし得るため、suiteは「pure production binaryではない」ことを検出しない。
- Evidence: `tools/test/check-c-sanitizer.sh:134-164`、`tests/phpt/001-load.phpt:34-60`、`tests/phpt/helpers.inc:86-99`、`grpc.c:80-82,157-159`。
- Expected model: production variantはbench diagnosticsとtest-only fault seamの双方がcompile outされ、runnerの固定期待へ照合される。
- Why it matters: test-fault seamはproduction transport pathへ分岐を追加する。現HEADには漏出がないが、production sanitizer laneのvariant regressionを静かに通す。
- Recommended fix: `GRPC_LITE_EXPECT_TEST_FAULT=0|1`または単一build-variant値をrunnerから渡し、PHPT 001でtest-fault MINFO rowも外部期待へ照合する。productionは0、bench+fault / test buildは1とする。

### REVIEW-20260713-005: PHPT 041はdetached connectionのdestroy漏れを検出しない

- Severity: `Low`
- Status: `Open`
- Reviewer role: C lifetime / sanitizer oracle adversary
- Finding: PHPT 041は`detach + destroy`をcontractとして記述するが、assertするのはexception、cache非残留、fresh connectionのpreface 132本、RST非送出である。mem-recv fatal branchの`destroy_detached_connection_if_unowned()`だけを削除してもcacheはdetach済みで各attemptはfresh connectionになるため全assertが通る。ASanは`detect_leaks=0`なので132個のconnection / fd / nghttp2 session leakも検出しない。
- Evidence: `tests/phpt/041-fatal-mem-recv-diagnostic-caller-lifetime.phpt:28-94`、`src/unary_call.c:229-237`、`tools/test/check-c-sanitizer.sh:36-40`。
- Expected model: callee-consumed `FAILURE` contractはcache detachだけでなく、最後のownerが消えたconnectionのterminal releaseまで固定する。
- Why it matters: destroyだけが退行すると現PHPTとASan/UBSan gateを通過し、長寿命workerでfailureごとにmemory / fd / sessionを漏らしてresource exhaustionへ進む。
- Recommended fix: test-only destroy counter / trace eventで132 created connectionすべてのdestroyをassertするか、PHPT内でboundedな`/proc/self/fd`差分を検査する。destroy-only mutationでtestがFAILすることを確認する。

### REVIEW-20260713-006: open issueが変更前の挙動と棄却済みplanを現状として残す

- Severity: `Low`
- Status: `Open`
- Reviewer role: domain model documentation adversary
- Finding: issue Backgroundはdeadline時にconnectionを破棄しRSTを送らない状態を「現状」と記載し、PlanはRST後の短時間drainをactive planとして残す。一方Progressでは実装済み、Decision Logではdrain不採用であり、同じ文書内でcurrent lifecycleが矛盾する。今回追加したVerificationも第六パスを第五パスより先に置いている。
- Evidence: `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md:15-17,38-43,47-52,62-75`。
- Expected model: historical problem / superseded planとcurrent implementation modelを明示的に区別し、verification historyは因果順に並べる。
- Why it matters: stream-local deadline cancellationとRST後lifecycleは元issueの中心概念であり、入口文書が正反対の現状を示すと後続作業が誤った前提から始まる。
- Recommended fix: Backgroundを「変更前」と明記し、Planを現行のRST submit/flush + reuse時preflight方針へ更新するかsupersededと表示する。第五パスの後に第六パスを置く。

## Adequate Fixes Confirmed

- sanitizer runnerはproduction（`--enable-grpc`のみ）とbench+faultを別buildとして順に実行し、bench layoutでproduction sanitizer coverageを置換しない。
- PHPT 001のbench期待値は被検moduleのMINFOから切り離され、runner宣言へ照合される。
- PHPT 041は1KiB response / 8-byte limit / `rst-submit-fatal`でcallback fatalから`nghttp2_session_mem_recv()` fatalへ到達し、diagnostic callerのpost-failure UAFとcache detach / non-reuseを固定する。
- 現HEADのmem-recv fatal implementationはdetach、owner clear、cleanup、unowned destroyを正しい順序で実行する。

## Verification

- HEAD: `199bf01f584fab4d35f42143fe57288e38a97fdd`
- `git diff --check 3081608..199bf01`: PASS
- `bash -n tools/test/check-c-sanitizer.sh tools/test/check-phpt.sh`: PASS
- ASan/UBSan sanitizer 2 lane: production 22 PASS / 4 SKIP、bench+fault 26/26 PASS、reportなし
- NTS PHPT: 26/26 PASS
- ZTS PHPT: 24 PASS / 2 SKIP、0 FAIL
- C unit: protocol/status/transport core 3/3 PASS
- PHPUnit: 31 tests / 116 assertions PASS
- C static analysis: PASS

## GitHub Delivery

- Posted one top-level PR Conversation comment with the three Low findings above:
  - https://github.com/dkkoma/php-grpc-lite/pull/29#issuecomment-4956180035
- Commit-pinned source links target HEAD `199bf01f584fab4d35f42143fe57288e38a97fdd`.
