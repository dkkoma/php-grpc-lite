# PR #29 adversarial re-review 統合版 2026-07-13（第六パス）

## Scope

- PR #29: base `e49d4be` / HEAD `3081608`
- 今回の対応commit: `e424689`（前回reviewed HEAD `287bc939` からの実装修正）
- 元issue: `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md`
- 統合元:
  - `docs/reviews/issues/2026-07-12-pr29-adversarial-lifetime-pass6.md`
  - `docs/reviews/issues/2026-07-12-pr29-adversarial-protocol-pass6.md`
  - `docs/reviews/issues/2026-07-12-pr29-adversarial-test-pass6.md`

## Reviewer Role

- `adversarial re-review consolidator / GitHub delivery editor`

## Review Result

- Decision: `REQUEST_CHANGES` 相当
- Open: `Medium 1 / Low 2`
- 前回のHigh（diagnostic caller UAF）とMedium（deadline details priority）はproduction codeとfocused regression上は修正済み。
- 残件は、今回追加したbench sanitizer coverageがproduction sanitizer laneを置換したこと、およびproduction/bench boundaryと追加FAILURE branchのregression oracle不足。

## Open Findings

### REVIEW-20260713-001: bench sanitizer追加でproduction extensionのsanitizer laneを置換している

- Severity: `Medium`
- Status: `Open`
- Finding: `check-c-sanitizer.sh`は従来のnon-bench extension buildへ`--enable-grpc-bench`を追加し、full PHPT sanitizer runをbench構成だけに置き換えた。bench macroはdiagnostic source/functionだけでなく`grpc_call` layoutとunary core signature/branchも変更するため、productionと同一binary shapeではない。
- Evidence: `tools/test/check-c-sanitizer.sh:87-150`、`src/grpc_exchange_state.h:28-30,73-88,112-114`、`src/unary_call.c:84-88,270-274`。CIのnon-bench coverage/ZTS laneはfull PHPTを実行するがsanitizerではなく、Crash/UB jobはextensionをloadしないpure `protocol_core` fuzzerである。
- Expected model: productionとdiagnosticは別build variantなので、bench regression sanitizerはproduction sanitizer gateへ加算し、置換しない。
- Why it matters: bench buildのASan PASSはstruct layout/source set/core call shapeが異なるproduction buildのmemory safetyを証明しない。今回bench-only UAFを見つけた事実自体がvariant別sanitizerの必要性を示す。
- Recommended fix: production-like（`--enable-grpc --enable-grpc-test-fault`）full PHPTと、bench+faultのfocused ownership PHPTを2 laneで実行する。少なくともmemory/lifetime変更では両variantをASan/UBSanへ通す。

### REVIEW-20260713-002: PHPT 001のproduction非露出期待値が被検moduleのMINFOに依存する

- Severity: `Low`
- Status: `Open`
- Finding: PHPT 001はbench functionの期待値を同じmoduleのMINFO行から算出するため、MINFOとfunction registrationの一致は検査するが、「production buildではbench surfaceが存在しない」という外部contractを検査しない。configure default/build flagが誤ってbenchを有効化し、MINFOとfunctionsが一緒に露出してもPASSする。
- Evidence: `tests/phpt/001-load.phpt:34-53`、`grpc.c:24-28,160-162`、`config.m4`。non-bench coverage laneも同じ自己適応oracleを使う。
- Expected model: production/benchの期待variantはrunner側から宣言し、MINFOとfunction surfaceの双方をその固定期待へ照合する。
- Why it matters: production/diagnostic分離はSPECのarchitecture boundaryだが、negative regressionがbuild誤設定を検出できない。現HEADのdefault production build自体には漏出を確認していない。
- Recommended fix: runnerから`GRPC_LITE_EXPECT_BENCH=0|1`等を渡してPHPT 001が固定期待へ照合するか、production surface専用PHPTとbench surface専用PHPTを分ける。

### REVIEW-20260713-003: PHPT 040が新たに変更したunary mem-recv FAILURE ownership branchを固定しない

- Severity: `Low`
- Status: `Open`
- Finding: `e424689`はsubmit fatalだけでなくstream registration failureと`nghttp2_session_mem_recv()` failureにもcallee側detach/destroyを追加したが、PHPT 040は`submit-request-fatal`だけを通る。既存`rst-submit-fatal` + oversized unary responseでdiagnostic callerからmem-recv FAILUREへ決定的に到達できる。
- Evidence: `src/unary_call.c:183-191,226-237`、`tests/phpt/040-fatal-submit-diagnostic-caller-lifetime.phpt:13-41`、`tests/phpt/038-fatal-rst-submit-marks-connection-dead.phpt`。focused ASan probeでは現実装が安全であることを確認したがrepository regressionには未固定。
- Expected model: ownership契約を変更した到達可能なterminal branchごとに、callee destroy後のcaller非参照とdead cache evictionをsanitizer regressionへ固定する。
- Why it matters:将来mem-recv branchだけdetachが欠落してもsame-key lazy evictionで表面上通りやすく、PHPT 040は契約分岐を検出しない。
- Recommended fix: bench diagnostic PHPTへ`rst-submit-fatal`、小さいreceive limit、大きいunary responseの2 attemptを追加し、期待exceptionのみ・ASan/UBSan reportなし・dead cache非残留を固定する。

## Adequate Fixes Confirmed

- unary coreはunusable化してFAILUREを返すsubmit/register/mem-recvの3経路でdetach、owner clear、unowned destroyをcallee内に完結し、production/diagnostic callerはFAILURE後pointerを再参照しない。
- deadline details resolverはserver由来`grpc-message`の既存priorityを維持した後、local timed-outをconnection cleanup errorより先に解決し、unary/server streaming双方でcode/detailsがdeadlineへ揃う。
- PHPT 038/040はbench+fault ASan/UBSanでPASSし、前回のUAFとdeadline details不整合は再現しない。
- default production buildはbench diagnostic functions/MINFOとtest-fault seamを露出しないことを固定期待の外部checkで確認済み。

## Verification

- HEAD: `3081608c361803af7a285c68d20536e68d63288a`
- `git diff --check 287bc939..3081608`: PASS
- bench+fault NTS PHPT: 25/25 PASS
- bench+fault ASan/UBSan PHPT: 25/25 PASS、sanitizer reportなし
- ZTS PHPT: 24 PASS / 1 SKIP（bench専用040）/ 0 FAIL
- C unit: protocol/status/transport core 3/3 PASS
- PHPUnit: 31 tests / 116 assertions PASS
- default production build: warningなし、load PASS、bench/test-fault surface非露出
- focused diagnostic unary mem-recv fatal ASan probe: 2 attempts PASS、sanitizer reportなし

## GitHub Delivery

- Posted one top-level PR Conversation comment with the Medium and two Low findings above:
  - https://github.com/dkkoma/php-grpc-lite/pull/29#issuecomment-4952931089
- Commit-pinned source links target HEAD `3081608c361803af7a285c68d20536e68d63288a`.
