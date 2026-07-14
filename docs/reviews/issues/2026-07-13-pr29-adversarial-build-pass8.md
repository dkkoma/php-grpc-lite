# PR #29 第八パス adversarial build-variant / production-boundary review 2026-07-13

## Scope

- `199bf01f..1faf80a1`（実装修正commit `3ed170f`）
- `config.m4`
- `grpc.c`
- `src/transport.h`
- `src/transport.c`
- `tests/phpt/001-load.phpt`
- `tests/phpt/038-fatal-rst-submit-marks-connection-dead.phpt`
- `tests/phpt/039-fatal-submit-request-marks-connection-dead.phpt`
- `tests/phpt/040-fatal-submit-diagnostic-caller-lifetime.phpt`
- `tests/phpt/041-fatal-mem-recv-diagnostic-caller-lifetime.phpt`
- `tests/phpt/helpers.inc`
- `tools/test/check-phpt.sh`
- `tools/test/check-c-coverage.sh`
- `tools/test/check-c-sanitizer.sh`
- `tools/test/check-zts-phpt.sh`
- `tools/test/check-crash-ub.sh`
- `.github/workflows/native-qa.yml`
- `.github/workflows/release-prebuilt-artifacts.yml`
- `docker/Dockerfile.release-artifact`
- `tools/release/build-prebuilt-artifact.sh`

## Reviewer Role

- PR adversary（build variant matrix / production-vs-bench・test-fault boundary / CI oracle）

## Review Prompt Summary

- 第七パスで追加された`GRPC_LITE_EXPECT_TEST_FAULT`が全PHPT runnerでbuild flagsと整合し、productionへのfault seam混入を独立に検出するか確認する。
- production / test-fault / bench / bench+faultの組合せ、Native QAとrelease artifactのcall siteを総当たりし、外部期待値が未実行または自己申告だけになるmutationを反証する。
- 元issueのdeadline / RST_STREAM対応中に導入されたfault seamとdiagnostic callerのproduction boundaryに限定する。

## Issues

### REVIEW-20260713-007: pure-productionの外部boundary oracleがPR CIで一度も実行されない

- Severity: `Medium`
- Status: `Rejected`
- Reviewer role: `PR adversary (build variant / CI gate)`
- Finding: `GRPC_LITE_EXPECT_TEST_FAULT=0`を渡すpure-production laneは`check-c-sanitizer.sh`内に追加されたが、Native QAはこのscriptを呼ばない。CIのNTS coverageとZTS PHPTはどちらも`--enable-grpc-test-fault` + `EXPECT_TEST_FAULT=1`であり、Crash/UB jobはextensionをbuild/loadせずpure `protocol_core` fuzzerだけを実行する。release artifactは`./configure --enable-grpc`でpure-productionをbuildするものの、確認は`extension_loaded()`とmetadata生成だけでPHPT 001相当のbench/fault非露出assertionを行わない。そのため例えば`config.m4`のtest-fault defaultが誤って`yes`へ変わるmutationは全PR CIがgreenのまま通り、release workflowもfault seam入り`grpc.so`を正常artifactとして公開できる。手動で`check-c-sanitizer.sh`を実行すれば検出できるが、merge/release gateにはなっていない。
- Evidence: `tools/test/check-c-sanitizer.sh:134-166`（唯一の`0/0` lane）、`.github/workflows/native-qa.yml:52-87,149-186`（coverage/ZTSはfault build）、同`:100-130`と`tools/test/check-crash-ub.sh`（extensionを扱わない）、`docker/Dockerfile.release-artifact:34-55`（`--enable-grpc` build後はload/metadataのみ）。repository全体で`GRPC_LITE_EXPECT_TEST_FAULT=0`または`run_phpt_lane production 0 0`を使うcall siteはsanitizer scriptの1箇所だけ。
- Expected model: shipped production variant（bench diagnosticsなし、test fault seamなし）のnegative boundaryは、任意の手動hardeningではなくPR CIまたはrelease artifact自身で必ず検査する。test buildのpositive coverageとproduction buildのnegative boundaryは別のgateであり、一方で他方を代用しない。
- Why it matters: test-fault seamは`GRPC_LITE_TEST_FAULT`によりrequest submit / policy RSTをfatal nghttp2 errorへ差し替えるproduction hot-path branchである。release artifactへ混入すると、環境変数の誤設定や注入で正常RPCを意図的なfatal failureへ変えられる。今回の外部oracle自体は正しいが、自動gateから到達不能ならproduction regressionをmerge前に止めない。
- Recommended fix: Native QAにcheapなpure-production boundary step/jobを追加し、`./configure --enable-grpc`でbuildしたmoduleに`GRPC_LITE_EXPECT_BENCH=0 GRPC_LITE_EXPECT_TEST_FAULT=0`を渡してPHPT 001（および下記negative behavior probe）を実行する。加えてrelease artifact DockerfileでもMINFOのbench/fault行とbench function非露出を固定期待でassertし、公開対象binaryそのものを検査する。full sanitizer 2 laneをCIへ追加する必要はなく、focused production build/load/boundary checkでよい。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: 全`run-tests.php` / configure / workflow call siteの静的matrix確認。現HEADの`config.m4` defaultは`no`でrelease configureも`--enable-grpc`のみのため、現在のartifactへ実際にseamが混入しているという指摘ではない。
- Notes: `check-native-release-hardening.sh`はsanitizer 2 laneを含むが、Native QAおよびrelease artifact workflowから呼ばれていない。再triageでは、現行production/release artifactは正しいflagsでbuildされ、PR内の手動sanitizer gateもpure-productionを実行するため、Native QAへの追加はこのdeadline/RST修正のcorrectnessに必須ではない一般的なCI hardeningと判断し、今回のGitHub投稿対象から外す。

### REVIEW-20260713-008: test-fault外部期待はMINFOを検査するだけでproductionで環境変数が無効なことを固定しない

- Severity: `Low`
- Status: `Open`
- Reviewer role: `PR adversary (mutation-resistant test oracle)`
- Finding: PHPT 001の`EXPECT_TEST_FAULT`はmoduleのMINFO rowがcompile macroと一致することだけを確認し、`GRPC_LITE_TEST_FAULT`を設定したproduction processでfault動作が本当にcompile outされているかを実行しない。fault専用PHPT 038〜041は`helpers.inc`で同じMINFO rowを見てproduction laneではSKIPする。このためMINFO guardを正しく保ったまま`src/transport.h`のproduction fallbackやfault call siteを退行させ、production buildが`GRPC_LITE_TEST_FAULT=submit-request-fatal`をhonorするようになっても、001はPASSし038〜041はSKIPする。外部期待値はbuildの表示を独立化したが、保護対象であるruntime seam非露出のnegative oracleにはなっていない。
- Evidence: `tests/phpt/001-load.phpt:41-52`（MINFO文字列のみ）、`tests/phpt/helpers.inc:86-99`（fault PHPTのavailabilityもMINFO由来）、`src/transport.h:91-97`（productionでは`grpc_lite_test_fault_enabled()`をconstant falseへする本来の境界）、`src/unary_call.c:162-180`、`src/server_streaming_call.c:127`、`src/transport.c:357,977`（fault predicateが実際に変えるruntime branch）。
- Expected model: production boundaryのcontractは「MINFOにseamを表示しない」だけでなく、「fault環境変数を与えても通常RPCの送信/cancel/policy pathを一切変更しない」。外部oracleはこのobservable behaviorもnegative testで固定する。
- Why it matters: 過去にproduction hot pathへfault seamが混入したことが実際のreview Highとなっており、表示行だけの検査では同じclassの退行を別のguard位置で再導入できる。現HEADのfallback macroは正しくconstant falseなのでruntime defectではなく、mutation-resistant regression oracle不足としてLowとする。
- Recommended fix: pure-production lane専用PHPTを追加し、`--ENV-- GRPC_LITE_TEST_FAULT=submit-request-fatal,rst-submit-fatal`でmoduleを起動したうえで通常unary（必要ならdeadline cancel / oversize policyも）を実行し、fault例外にならず本来のstatus/wire outcomeになることをassertする。test-fault有効laneではrunner期待値を見てSKIPしてよいが、SKIP判定を被検MINFOから導かない。production fallbackまたは代表call siteをfault有効へするmutationでこのtestがFAILすることを確認する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: source-level mutation analysis。現HEADでは`PHP_GRPC_LITE_ENABLE_TEST_FAULT`未定義時にpredicateがconstant falseへmacro展開され、実際のseam実装とMINIT initもcompile outされることを確認した。
- Notes: fault有効laneのpositive behavior / exact-token / MINIT snapshotはPHPT 038〜041で既に十分に固定されており、本指摘はfault無効側のnegative behaviorだけを対象とする。

### REVIEW-20260713-009: 独立したbench-only build variantがどのrunnerにも存在しない

- Severity: `Low`
- Status: `Rejected`
- Reviewer role: `PR adversary (build variant matrix)`
- Finding: `--enable-grpc-bench`と`--enable-grpc-test-fault`は独立したconfigure optionだが、repositoryのPHPT/build matrixは`bench=0,fault=0`（sanitizer production）、`0,1`（coverage/ZTS）、`1,1`（standard PHPT/sanitizer bench-fault）だけで、通常のdiagnostic構成である`1,0`を一度もbuild/testしない。今回の外部期待値は`EXPECT_BENCH=1, EXPECT_TEST_FAULT=0`を表現できるが、その組合せを渡すcall siteがない。したがってbench optionが誤ってtest-faultを暗黙有効化するconfig mutationや、fault predicateがconstant falseへcompileされるbench-only code shapeのbuild/runtime regressionは既存3 variantがすべてPASSしても検出されない。
- Evidence: `config.m4:1-18`（2 optionは独立）、`tools/test/check-c-sanitizer.sh:165-166`（`0/0`, `1/1`）、`tools/test/check-c-coverage.sh:19,94`と`tools/test/check-zts-phpt.sh:18,61`（`0/1`）、`tools/test/check-phpt.sh:15,55-56`（`1/1`）。repository内に`EXPECT_BENCH=1`かつ`EXPECT_TEST_FAULT=0`のrunner call siteはない。
- Expected model: 公開された独立configure optionの各shipping/diagnostic variantは、少なくともbuild/loadと外部surface expectationを1回通す。fault seamを使うregression laneをbench diagnosticsの唯一の検証binaryにしない。
- Why it matters: bench buildは`grpc_call` layout、unary core signature/branch、diagnostic source/function tableをproductionから変える一方、test-fault buildはsubmit/RST branchとMINIT stateを追加する。`1/1`のPASSは`1/0`という実利用構成のcompile/link/surface boundaryを証明しない。production artifactへの直接影響はないためLowとする。
- Recommended fix: focused bench-only lane（`./configure --enable-grpc --enable-grpc-bench`、`EXPECT_BENCH=1 EXPECT_TEST_FAULT=0`）を追加し、PHPT 001と通常のdiagnostic unary smokeを実行する。sanitizer full suiteをさらに増やす必要はなく、standard build matrixのfocused checkで十分。optionを独立にsupportしない方針なら、config/docsでbenchがtest-faultを含む単一diagnostic variantだと明示してoption modelを統合する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: 全configure / run-tests call siteの静的matrixを作成し、`1/0`不在を確認した。
- Notes: fault injectionを必要とするPHPT 040/041を`1/1`で維持すること自体は正しい。再triageでは、bench-only optionは本issue以前から存在するgeneral diagnostic build variantであり、今回のdeadline/RST変更に必要なfault-induced regressionは`1/1`で到達できているため、`1/0` matrix追加は元issue外の一般hardeningと判断してGitHub投稿対象から外す。

## Adequate Fixes Confirmed

- 既存runner内の`GRPC_LITE_EXPECT_TEST_FAULT`値はbuild flagsと一致する。sanitizer productionは`bench=0/fault=0`、sanitizer bench-faultとstandard NTS PHPTは`1/1`、coverageとZTSはbench未設定の`0` / fault=`1`であり、単独のflag取り違えはPHPT 001をFAILさせる。
- PHPT 001はbenchについてMINFO rowだけでなく全5 diagnostic functionも同じ外部期待へ照合し、test-faultについてはMINFO rowを外部期待へ照合する。未設定を非露出へ倒すdefaultも安全側である。
- `wire.connection_close`とPHPT 041の132 close assertionにより、mem-recv fatal ownership regressionはcache detachだけでなくconnection destroyまで独立に観測する。destroy-only mutationがFAILするというauthor verificationとも整合する。
- issue Background / Plan / Verificationは変更前・現在・superseded plan・第五→第六→第七パスの時系列を区別するよう修正され、第七パスの文書指摘は解消している。
- 現HEADのproduction sourceはtest fault predicateをconstant falseへcompileし、MINIT init / fault parserもcompile outする。今回のfindingsはcurrent runtime leakではなく、build matrixと自動regression oracleの残り穴である。

## Scope Triage

- Current runtime / release defect: `none`。現HEADのproductionとrelease artifactはbench/test-faultを有効にせず、fault predicateもcompile outされる。
- 今回postすべきrequired regression gate: `REVIEW-20260713-008`（Low）のみ。PR #29のreview remediationがproduction hot pathへ新設したfault seamについて、productionでは環境変数を与えてもbehaviorally inertというcontractを直接固定するため、元issueから逸脱しない。
- 今回postしないgeneral hardening: `REVIEW-20260713-007`（CIへのpure-production lane常設）と`REVIEW-20260713-009`（bench-only `1/0` matrix）。どちらも改善としては妥当だが、現在のdeadline/RST実装またはrelease binaryの欠陥ではなく、PR #29の完了条件に必須ではない。

## Review Result

- Blocker: none
- High: none
- Medium: none Open / 1 Rejected (out of scope)
- Low: 1 Open / 1 Rejected (out of scope)
- Design Decision: none

## Verification Notes

- HEAD: `1faf80a150dedbbf278a6cb4fbbf914ebb687b69`
- `git diff --check 199bf01..1faf80a`: PASS
- `bash -n`（PHPT/sanitizer/coverage/ZTS/Crash-UB/release artifact runner）: PASS
- Docker/build execution: parent verificationへ委譲（本reviewでは指示どおり未実行）
