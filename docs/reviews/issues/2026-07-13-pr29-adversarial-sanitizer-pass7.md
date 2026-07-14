# PR #29 第七パス adversarial sanitizer / build-boundary review 2026-07-13

## Scope

- `3081608c..199bf01f`（実装修正commit `24abba3`）
- `tools/test/check-c-sanitizer.sh`
- `tools/test/check-phpt.sh`
- `tools/test/check-c-coverage.sh`
- `tools/test/check-zts-phpt.sh`
- `.github/workflows/native-qa.yml`
- `tests/phpt/001-load.phpt`
- `tests/phpt/041-fatal-mem-recv-diagnostic-caller-lifetime.phpt`
- `src/unary_call.c`（PHPT 041が固定すべきownership branch）

## Reviewer Role

- PR adversary（sanitizer / test oracle / production-vs-diagnostic build boundary）

## Review Prompt Summary

- 第六パスのMedium（production sanitizer laneをbench laneで置換）、Low（PHPT 001の循環oracle）、Low（mem-recv fatal ownership branchのregression不足）が実際に解消したかを確認する。
- production / bench+fault / coverage / ZTS各runnerのbuild flagと期待値伝播を突き合わせ、元issueおよびreview remediationのownership scope内で新しい検証穴がないか反証する。

## Issues

### REVIEW-20260713-004: PHPT 041はdetachを固定するがdetached connectionのdestroyを観測しない

- Severity: `Low`
- Status: `Open`
- Reviewer role: `PR adversary (sanitizer / test oracle)`
- Finding: PHPT 041はコメントとreview記録でmem-recv fatal時の「detach + destroy」を固定すると説明しているが、実際のoracleはdead entryがpersistent cacheへ残らないことと、次attemptが新しいprefaceを送ることだけを観測する。`src/unary_call.c` のmem-recv fatal branchから `destroy_detached_connection_if_unowned(connection)` だけを削除しても、`detach_persistent_connection_by_ptr()` によってcacheは空のままなので130-key sweepは通り、全attemptは新connectionを作るのでpreface 132本のassertionも通る。callerはpointerを再参照しないためASan UAFも起きず、sanitizer runnerは`detect_leaks=0`なのでconnection object / nghttp2 session / socketのleakを報告しない。
- Evidence: `src/unary_call.c:229-237`（detachは231行、destroyは235行の別操作）、`tests/phpt/041-fatal-mem-recv-diagnostic-caller-lifetime.phpt:56-94`（exception、cache exhaustion非発生、preface本数のみ）、`tools/test/check-c-sanitizer.sh:36-40`（ASan `detect_leaks=0`）。PHPT 041の92行目もpreface本数から「destroyed」と記述しているが、wire上の新規connectionは前connectionがcacheから外れたことしか証明しない。
- Expected model: fatal `nghttp2_session_mem_recv()` のFAILURE branchはpersistent cacheからconnectionをdetachした後、ownerをclearし、unownedになったconnectionを必ずdestroyする。regression oracleはcache非残留だけでなくprocess内のsocket / connection resourceがattemptごとに回収されることも観測する。
- Why it matters: この1行が退行すると、fatal callbackを受けるたびにconnection object、nghttp2 session、recv/write buffer、socket fdがworker processの寿命まで失われる。132回のsweepを行う現PHPT自身でも同数のfd leakを見逃し、長寿命workerではfd exhaustionへ進み得る。現HEADのproduction codeが実際にdestroyしていることへの指摘ではなく、追加したregressionがownership契約の半分を固定していないという指摘である。
- Recommended fix: PHPT 041でattempt群の前後にprocessのopen fd数（Docker/CIのLinuxでは`/proc/self/fd`）を測り、少数の許容差内に戻ることをassertするか、test buildだけのconnection destroy counter / trace eventを用意して全132 connectionのdestroyを直接確認する。少なくともpreface本数をdestroyの証拠とは扱わない。修正後は235行相当のdestroy callを一時除去したmutationでPHPT 041がfd/resource assertionによりFAILすることを確認する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: 現HEADのコード読解とoracle mutation analysis。`detach_persistent_connection_by_ptr()`除去mutationがFAILするというauthorの確認はcache retentionへの検出力だけを示し、destroy-only mutationへの検出力は示さない。
- Notes: LeakSanitizerをPHP全体へ一律有効化することは外部library由来noiseを伴うため要求しない。PHPT 041内のboundedなfd差分またはtest-only destroy観測で十分。

## Adequate Fixes Confirmed

- 第六パスMediumは解消。`check-c-sanitizer.sh`は純production（`--enable-grpc`のみ）とbench+fault（`--enable-grpc --enable-grpc-test-fault --enable-grpc-bench`）を別buildとして順に実行し、production layoutをbench layoutで置換しない。
- 第六パスのPHPT 001 Lowは解消。bench期待値は被検moduleのMINFOではなく`GRPC_LITE_EXPECT_BENCH`から取得し、同じ外部期待へMINFO rowと全bench functionを照合する。bench runnerは明示的に`1`、production sanitizer laneは明示的に`0`、coverage / ZTSのnon-bench laneは未設定時の安全側default `false`を使う。
- PHPT 041は`rst-submit-fatal` + 8-byte receive limit + 1KiB responseでmem-recv fatal branchへ到達し、diagnostic callerのcallee-consumed pointer非参照とdead cache entryの非残留を固定する。この2点については第六パスLowへのadequateな追加coverageである。

## Review Result

- Blocker: none
- High: none
- Medium: none
- Low: 1 (Open)
- Design Decision: none

## Verification Notes

- HEAD: `199bf01f584fab4d35f42143fe57288e38a97fdd`
- `git diff --check 3081608..199bf01`: PASS
- `bash -n tools/test/check-c-sanitizer.sh tools/test/check-phpt.sh tools/test/check-c-coverage.sh tools/test/check-zts-phpt.sh`: PASS
- `./tools/test/check-c-sanitizer.sh`: production lane 22 PASS / 4 SKIP、bench+fault lane 26/26 PASS、ASan/UBSan reportなし、exit 0
- `./tools/test/check-phpt.sh`: NTS bench+fault 26/26 PASS
- `./tools/test/check-zts-phpt.sh`: 24 PASS / 2 SKIP、0 FAIL
