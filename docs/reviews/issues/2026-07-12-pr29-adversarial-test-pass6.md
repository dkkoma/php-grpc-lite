# PR #29 第六パス adversarial test / build-boundary review 2026-07-12

## Scope

- `287bc939..3081608`（実装commit `e424689`）
- `grpc.c`
- `src/diagnostic/bench.c`
- `src/transport.c`
- `src/unary_call.c`
- `tests/phpt/001-load.phpt`
- `tests/phpt/038-fatal-rst-submit-marks-connection-dead.phpt`
- `tests/phpt/040-fatal-submit-diagnostic-caller-lifetime.phpt`
- `tests/phpt/helpers.inc`
- `tools/test/check-phpt.sh`
- `tools/test/check-c-sanitizer.sh`
- `tools/test/check-c-coverage.sh`
- `tools/test/check-zts-phpt.sh`
- `.github/workflows/native-qa.yml`

## Reviewer Role

- PR adversary（test oracle / sanitizer / production-vs-diagnostic build boundary）

## Review Prompt Summary

- 第五パスのHigh（diagnostic unary callerがcallee破棄済みconnectionをfailure後に再参照するUAF）とMedium（deadline detailsがsecondaryなRST submit fatalで上書きされる）が修正されたか、追加PHPTの検出力を含めて再確認する。
- `FAILURE`時のconnection lifetime契約へ揃えるために変更された全branch、bench/test-fault build、default production build、ZTS buildの境界を確認する。
- `--enable-grpc-bench`を標準PHPT/sanitizer runnerへ追加したことが、production構成の継続検証を弱めていないかCIを含めて確認する。

## Issues

### REVIEW-20260712-001: sanitizer gateがproduction layoutをbench layoutで置換している

- Severity: `Medium`
- Status: `Open`
- Reviewer role: `PR adversary (test / build-boundary)`
- Finding: `check-c-sanitizer.sh`は従来のnon-bench extension構成へ`--enable-grpc-bench`を追加し、full PHPT sanitizer runをbench構成だけに置き換えた。bench buildはdiagnostic source/functionを追加するだけではなく、`grpc_call`のfield layoutとunary coreの引数・条件分岐も変更するため、production構成と同一binary shapeではない。`check-c-coverage.sh`とZTS jobはnon-bench full PHPTを実行するがsanitizerではなく、`native-qa.yml`のCrash/UB jobはpure `protocol_core` fuzzerだけでextension PHPT sanitizerを実行しない。したがって、今回必要になったbench sanitizer coverageは得られた一方、既存のproduction-like extension sanitizer coverageが消えている。
- Evidence: `tools/test/check-c-sanitizer.sh:87-150`（`--enable-grpc-bench`を含む1 buildだけ）、`src/grpc_exchange_state.h:28-30,73-88,112-114`（bench時に`grpc_call` layout変更）、`src/unary_call.c:84-88,270-274`（bench時にcore signature/branch変更）、`.github/workflows/native-qa.yml:120-148`（Crash/UBは`check-crash-ub.sh`のみ）、`tools/test/check-crash-ub.sh`（extensionをbuild/runせずpure core fuzzのみ）。
- Expected model: productionとdiagnosticは別build variantなので、diagnostic回帰のsanitizer gateはproduction sanitizer gateへ加算する。少なくともmemory/lifetimeを扱うPRでは、production layoutとbench layoutの両方をASan/UBSanで通す。
- Why it matters: `PHP_GRPC_LITE_ENABLE_BENCH`でstruct layout・source set・core call shapeが変わるため、一方のASan PASSは他方のmemory safetyを証明しない。このPRでbench-only UAFが実際に見つかったこと自体がbuild variant別検証の必要性を示しており、逆方向のproduction-only regressionを現在のsanitizer runnerは検出できない。
- Recommended fix: `check-c-sanitizer.sh`をproduction-like（`--enable-grpc --enable-grpc-test-fault`）とbench+faultの2構成で実行する。実行時間を抑えるならproduction-likeでfull PHPT、bench+faultでPHPT 001/040とownership関連のfocused setを実行するmatrixに分ける。`check-phpt.sh`もproduction laneを置換せず、bench回帰用laneを追加する形が望ましい。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: 現HEADのbench+fault ASan/UBSan full suiteは25/25 PASS。別途default production buildはwarningなしでbuild/loadでき、diagnostic function/MINFO非露出を直接assertしてPASSした。ただし後者はsanitizerなし。
- Notes: non-bench core signatureの通常動作はCIの`check-c-coverage.sh`（bench無効、test-fault有効）でfull PHPT対象になっているため、「production経路が自動テストから完全に消えた」という指摘ではない。欠落はproduction variantのsanitizer coverageである。

### REVIEW-20260712-002: PHPT 001がproduction非露出の期待値をMINFO出力自身から導いている

- Severity: `Low`
- Status: `Open`
- Reviewer role: `PR adversary (test / build-boundary)`
- Finding: PHPT 001はbench functionの期待値を、同じmoduleのMINFOに`grpc_lite bench diagnostics`行があるかどうかから決める。これは「MINFOとfunction registrationが一致する」ことは検査するが、「`--enable-grpc`だけのproduction buildではbench surfaceが存在しない」という外部のbuild期待値を検査しない。configure defaultやbuild flagが誤ってbenchを有効にし、MINFOとfunctionが一緒に露出した場合もPASSする。
- Evidence: `tests/phpt/001-load.phpt:34-53`（`$benchBuild`を`phpinfo()`から算出し、その値をfunction期待値に再利用）、`grpc.c:24-28,160-162`（function tableとMINFO rowが同じcompile macroに従う）、`config.m4`（同じconfigure optionでmacroとdiagnostic sourcesを有効化）。`check-c-coverage.sh`のnon-bench laneも同じ自己適応oracleを使うため、この点はnon-bench CI laneの存在だけでは補えない。
- Expected model: production/benchの期待variantは被検moduleの出力ではなくrunner側から宣言し、PHPTは宣言されたvariantとMINFO/function surfaceの双方を比較する。
- Why it matters: production/diagnostic separationは`docs/SPEC.md`の明示的なarchitecture boundaryだが、現在のnegative regression testはその境界の誤設定を検出できない。現HEADのdefault production build自体は正しいため、これは現行動作不良ではなくoracleの弱化である。
- Recommended fix: runnerから例として`GRPC_LITE_EXPECT_BENCH=0|1`を渡し、PHPT 001でMINFO rowと全bench functionをその明示期待値へ照合する。あるいはproduction surface専用PHPTを常にbench無効laneで実行し、bench surface用の別PHPTをbench laneで実行する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: default production build（`./configure --enable-grpc`）でMINFO row/test-fault rowとbench function 5件がすべて非露出であることをPHPT外の固定期待値で確認し、PHPT 001もPASS。bench+fault buildではPHPT 001がPASS。
- Notes: 現HEADのbuild boundary実装に漏出は確認していない。

### REVIEW-20260712-003: PHPT 040が変更されたもう一つの到達可能なFAILURE ownership branchを固定していない

- Severity: `Low`
- Status: `Open`
- Reviewer role: `PR adversary (test / build-boundary)`
- Finding: `e424689`はsubmit-request fatalだけでなく、unaryのstream registration failureと`nghttp2_session_mem_recv()` fatalでもcallee側detach/destroyを完結させるよう変更した。しかしPHPT 040は`submit-request-fatal`だけを通る。既存の`rst-submit-fatal` seamとoversized unary responseを組み合わせれば、diagnostic callerから到達できる`nghttp2_session_mem_recv()` FAILURE branchも決定的に通せるが、repository testには固定されていない。stream registration failureは現行seamからは到達不能である。
- Evidence: `src/unary_call.c:183-191,226-237`（今回detach/destroyを追加した2 branch）、`tests/phpt/040-fatal-submit-diagnostic-caller-lifetime.phpt:13-41`（`submit-request-fatal`のみ）、`tests/phpt/038-fatal-rst-submit-marks-connection-dead.phpt:43-97`（unaryはdeadlineのSUCCESS result、callback fatalはserver streamingであり、diagnostic unary mem-recv FAILURE callerを通らない）。
- Expected model: ownership契約を修正した到達可能な各terminal branchについて、calleeがconnectionを破棄してもcallerが再参照せず、dead cache entryも残さないことをsanitizer regressionで固定する。
- Why it matters: diagnostic callerのfailure後参照を削除した結果、今後mem-recv branchのdetachが欠落しても同一keyの次callがlazy evictionして表面上通りやすい。異なるfatal branchで契約が再び分岐しても、現行PHPT 040は検出しない。
- Recommended fix: `GRPC_LITE_TEST_FAULT=rst-submit-fatal`、小さい`max_receive_message_length`、大きいunary responseを使うbench diagnostic PHPTを追加し、2回以上のattemptが期待exceptionだけで終了してASan/UBSan reportがないことを固定する。可能ならauthorityを変えた複数keyでもcacheを占有しないことをassertする。registration failureは別fault seamを増やす費用と到達可能性を比較して判断する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: 現HEADのbench+fault ASan/UBSan buildで、`grpc_lite_unary()` + `rst-submit-fatal` + `max_receive_message_length=8` + 1024-byte responseを2 attempt実行し、両方が`nghttp2_session_mem_recv failed`だけを投げて終了（exit 0、sanitizer reportなし）。したがって現実装のbranchは安全で、指摘は回帰oracle不足に限定される。
- Notes: ZTS runnerはbenchを有効にしないためPHPT 040が1 SKIPになるが、production ZTS surfaceを主対象とする既存laneとしては妥当であり、独立指摘にはしていない。

## Review Result

- Blocker: none
- High: none
- Medium: 1 (Open)
- Low: 2 (Open)
- Design Decision: none

## Verification Notes

- `git diff --check 287bc939..3081608`: PASS。
- `./tools/test/check-c-sanitizer.sh`: C unit 3/3 PASS、bench+fault ASan/UBSan PHPT 25/25 PASS、sanitizer reportなし。
- `./tools/test/check-phpt.sh`: bench+fault NTS PHPT 25/25 PASS。
- `./tools/test/check-zts-phpt.sh`: 24 PASS / 1 SKIP（PHPT 040、bench無効のため）/ 0 FAIL。
- default production build（`./configure --enable-grpc`）: warningなし、load PASS、bench diagnostic function 5件・bench MINFO row・test-fault MINFO rowがすべて非露出。
- default production build focused PHPT: 001 PASS、038/039/040はtest-fault無効のため3 SKIP、0 FAIL。
- focused ASan probe（diagnostic unary deadline + `rst-submit-fatal`）: deadline resultを返してexit 0、sanitizer reportなし。SUCCESS pathはcalleeでdetachせずcallerがunusable connectionをevictするため、第五パスUAFと同型の再参照にはならない。
- focused ASan probe（diagnostic unary oversized response + `rst-submit-fatal`、2 attempts）: `nghttp2_session_mem_recv failed`を2回観測、exit 0、sanitizer reportなし。
