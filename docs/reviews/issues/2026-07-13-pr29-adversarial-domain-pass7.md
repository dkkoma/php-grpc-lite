# PR #29 第七パス HTTP/2 / gRPC domain model review 2026-07-13

## Scope

- `3081608c..199bf01f`（対応commit `24abba3`、レビュー記録commit `199bf01`）
- `tools/test/check-c-sanitizer.sh`
- `tools/test/check-phpt.sh`
- `tests/phpt/001-load.phpt`
- `tests/phpt/041-fatal-mem-recv-diagnostic-caller-lifetime.phpt`
- `src/unary_call.c` / `src/diagnostic/bench.c` / `src/transport.c`（ownership・fault seamの隣接実装）
- `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md`
- `docs/reviews/issues/2026-07-11-deadline-rst-keep-connection-domain-review.md`

## Reviewer Role

- HTTP/2 / gRPC lifecycle・production/diagnostic boundary・test oracle・current-state documentation adversary

## Review Prompt Summary

- 第六パスのMedium（production sanitizer laneのbench置換）とLow 2件（bench期待値の循環、mem-recv fatal ownership regression不足）への修正を再確認する。
- production / bench / test-faultのbuild boundary、unary `FAILURE`時のconnection ownership、fatal callbackから`nghttp2_session_mem_recv()` unwindまでのlifecycle、cache非残留oracle、issue/review文書の現状と時系列を敵対的に監査する。
- 元issueのdeadline / RST_STREAM / persistent connection lifecycleと、そこから直接導入されたremediation/test boundaryの範囲に限定する。

## Issues

### REVIEW-20260713-004: production sanitizer laneがtest-fault seam非露出を外部oracleで固定していない

- Severity: `Low`
- Status: `Open`
- Reviewer role: `HTTP/2 / gRPC domain model adversary`
- Finding: PHPT 001はbench surfaceについてはrunner由来の`GRPC_LITE_EXPECT_BENCH`へ外部化されたが、同じproduction boundaryを構成するtest-fault seamには対応する外部期待値がない。sanitizer production laneは現在`--enable-grpc`だけで正しくbuildされているものの、将来誤って`--enable-grpc-test-fault`が追加されても、038/039等はmodule自身のMINFOを見てSKIPを解除して実行されるだけで、suite全体はPASSし得る。つまり「pure production binary（fault seamなし）」というlaneの宣言は、bench非露出だけしかnegative assertionで固定されていない。
- Evidence: `tools/test/check-c-sanitizer.sh:134-164`（production laneをno fault/no benchと宣言するが外部入力は`expect_bench`だけ）、`tests/phpt/001-load.phpt:34-60`（bench MINFO/functionだけを外部期待と照合）、`tests/phpt/helpers.inc:86-99`（test-fault availabilityを同一moduleのMINFOから導出）、`grpc.c:80-82,157-159`（fault seam codeとMINFO rowが同じcompile macroに従う）。
- Expected model: production variantの外部不変条件は、bench diagnosticsとtest-only fault seamの双方がcompile outされていること。diagnostic/test laneは逆に必要なsurfaceが明示的に有効であることをrunnerから宣言し、被検module自身の出力を期待値にしない。
- Why it matters: test-fault seamはsubmit/RST/parserのproduction hot pathへ分岐を追加するため、production binaryへ混入すれば単なる追加PHP functionより境界違反が大きい。現HEADのproduction buildに漏出はないが、2-lane sanitizer gateが将来productionではないbinaryへ静かに変わる回帰を検出できない。
- Recommended fix: 例えば`GRPC_LITE_EXPECT_TEST_FAULT=0|1`（または単一の明示build-variant値）を各runnerから渡し、PHPT 001で`grpc_lite test fault seam` MINFO rowを外部期待へ照合する。sanitizer productionは0、bench-fault / NTS / ZTS / coverageのtest buildは1、bare productionは未設定=0とする。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: 現在の`run_phpt_lane production 0 --enable-grpc`と`config.m4` defaultはfault/benchを無効化しており、実装上の現行漏出はないことをsource reviewで確認。指摘はbuild-boundary regression oracleの片側不足に限定される。
- Notes: 第六パスのbench oracle修正自体はadequate。同じ外部期待モデルを、よりproduction-sensitiveなtest-fault seamにも揃える指摘である。

### REVIEW-20260713-005: open issueが変更前の挙動と棄却済みplanを現状として残し、Verificationのパス順も逆転している

- Severity: `Low`
- Status: `Open`
- Reviewer role: `HTTP/2 / gRPC domain model adversary`
- Finding: 更新されたopen issueは、Backgroundで依然として「現状の実装」がdeadline時にconnectionを破棄しRST_STREAMを送らないと記載し、Planでは短時間drainとdrain失敗時のconnection破棄を実施予定としている。しかし同じ文書のProgressではRST helper実装済み、Decision Logではdrainを実装しないと確定しており、current stateとhistorical problem/abandoned planが区別されていない。またVerificationは第一〜第四パスの後に第六パス、その後に第五パスという順で、実施順を逆転させている。
- Evidence: `docs/issues/open/2026-07-08-deadline-rst-stream-keep-connection.md:15-17,26`（変更前の挙動を「現状」と記載）、`:38-43`（drainをactive Planとして記載）、`:47-52,71`（実装済みかつdrain不採用）、`:62-67`（第一〜第四→第六→第五のVerification順）。
- Expected model: open issueのBackgroundは歴史的problem statementなら「変更前」と明示し、Planは現在採用された実装方針か明示的なsuperseded状態を示す。進捗・検証履歴は読み手が因果順を誤認しない一貫した順序にする。過渡的なreview historyは既存どおり`docs/reviews/`へ残す。
- Why it matters: deadlineをstream-local failureとして扱うのかconnection failureとして扱うのか、RST後にdrainするのかは本PRの中心的domain modelである。issue入口が正反対の「現状」と棄却済み手順を示すと、後続の修正・レビューが誤ったlifecycle前提から始まる。runtime defectではないためLowとする。
- Recommended fix: Backgroundの「現状」を「変更前」へ直し、Planを実装済みのRST submit/flush + preflight drain方針へ更新するか「当初Plan（superseded）」と明示する。Verificationは第五パスの後に第六パスを置く。Decision Logとreview issueには履歴をそのまま保持する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: 現HEADの`cancel_grpc_call_stream()`とPHPT 033、およびissue内Decision Logを相互確認し、文書の「現状」/drain planが現在の実装モデルではないことを確認。
- Notes: 第六パスのreview record本体（REVIEW-20260713-001〜003）の件数・Fix commit・Verification記載は整合している。

## Adequate Fixes Confirmed

- `check-c-sanitizer.sh`はproduction lane（`--enable-grpc`のみ）とbench+fault laneを別buildとして順に実行し、各laneでclean/build/load/full PHPTを行う。bench macroによる`grpc_call` layout・unary core signature・diagnostic source setの差をproduction sanitizerから排除したため、前回Mediumへの実装修正はadequate。
- PHPT 001のbench期待値はmodule MINFOから切り離され、runnerが`GRPC_LITE_EXPECT_BENCH=0|1`を宣言する。MINFO rowと全5 diagnostic functionを同じ外部期待へ照合し、未設定はproduction非露出側へ倒れるため、前回のbench oracle Lowは解消している。
- PHPT 041は`max_receive_message_length=8`に対する1KiB応答でcallback内policy RSTへ到達し、`rst-submit-fatal`が`NGHTTP2_ERR_CALLBACK_FAILURE`を返して`nghttp2_session_mem_recv()` fatal branchを通る。2 attemptでdiagnostic callerのpost-failure dereferenceをASan対象にし、authorityを含むconnection keyを130種類変えるsweepとpreface 132本でcache detach/destroyも固定する。bench+fault sanitizer laneに含まれるため、前回のmem-recv regression Lowへの修正はadequate。
- `3081608..199bf01`にはC source変更がなく、unary coreの契約（unusableにした`FAILURE` branchがdetach・owner clear・unowned destroyを完結し、callerはpointerを再参照しない）は維持されている。RST submit fatalはconnectionをdead化してcallbackをunwindし、mem-recv fatal cleanup後にconnectionを再駆動しない。

## Verification

- `git diff --check 3081608c..199bf01f`: PASS
- `bash -n tools/test/check-c-sanitizer.sh tools/test/check-phpt.sh`: PASS
- source review: configure macro/source boundary、sanitizer 2 lane、PHPT 001 external oracle、PHPT 041のfault→callback unwind→mem-recv fatal→detach/destroy、diagnostic caller、connection key/cache limitを相互確認
- docs review: issue Background / Plan / Progress / Verification / Decision Log、および第六パスreview recordの件数・Fix commit・verificationを相互確認

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: 2 (Open)
- Design Decision: none
