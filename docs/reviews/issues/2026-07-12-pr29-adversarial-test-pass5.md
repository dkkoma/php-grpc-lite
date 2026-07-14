# PR #29 第五パス adversarial test / build-boundary review 2026-07-12

## Scope

- `2af2d58..287bc939`（実装commit `3e128d4`）
- `config.m4`
- `src/unary_call.c`
- `src/diagnostic/bench.c`
- `tests/phpt/024-control-semantics.phpt`
- `tests/phpt/038-fatal-rst-submit-marks-connection-dead.phpt`
- `tests/phpt/039-fatal-submit-request-marks-connection-dead.phpt`
- `tests/phpt/helpers.inc`
- `tools/test/check-phpt.sh`
- `tools/test/check-c-sanitizer.sh`
- `tools/test/check-c-coverage.sh`
- `tools/test/check-zts-phpt.sh`

## Reviewer Role

- PR adversary（test oracle / sanitizer / production-vs-diagnostic build boundary）

## Review Prompt Summary

- 第四パスの3指摘（fault seamのproduction混入とrequest跨ぎUAF、partial message connection breakのcall-kind不一致、fatal submit後のdistinct-key cache枯渇）が、build boundaryと決定的なPHPT oracleを含めて修正されたか再確認する。
- `--enable-grpc-bench` と `--enable-grpc-test-fault` を併用した診断buildも、fatal submit後のconnection ownershipを安全に保つかASan/UBSanで確認する。

## Issues

### REVIEW-20260712-001: unary coreが破棄したdetached connectionをdiagnostic callerが再参照する

- Severity: `High`
- Status: `Open`
- Reviewer role: `PR adversary (test / build-boundary)`
- Finding: `nghttp2_submit_request` のfatal時、`grpc_lite_unary_call_perform_core_on_connection()` はcacheからconnectionをdetachし、ownerをclearした後に `destroy_detached_connection_if_unowned()` でconnectionを破棄して `FAILURE` を返す。しかし `--enable-grpc-bench` の `grpc_lite_unary()` callerは、戻り値が `FAILURE` の分岐で同じraw pointerを `connection_usable()` に渡す。このため `--enable-grpc-bench --enable-grpc-test-fault` の組合せでは、`submit-request-fatal` を1回注入するだけでheap-use-after-freeになる。
- Evidence: `src/unary_call.c:158-173`（`detach_persistent_connection_by_ptr` → owner clear → `destroy_detached_connection_if_unowned`）、`src/diagnostic/bench.c:1878-1880`（core failure後の `connection_usable(connection)`）。ASan/UBSan buildで `GRPC_LITE_TEST_FAULT=submit-request-fatal` を設定し `grpc_lite_unary("pass5-key", "test-server", 50051, "/helloworld.Greeter/BenchUnary", "")` を1回呼ぶと、freeは `src/unary_call.c:171` / `src/transport.c:221`、useは `src/diagnostic/bench.c:1879` / `src/transport.c:873` としてdeterministicにabortした（exit 133）。
- Expected model: connectionのdetach / 最終destroyを行う層と、callerが保持するraw pointerのlifetime contractを一致させる。calleeが破棄して返るならcallerはfailure後にpointerへ触れない。callerが後処理を担うならcalleeは破棄せず、所有権移譲を型または明示的な結果で伝える。
- Why it matters: ASanで再現するheap-use-after-freeであり、非sanitizerのdiagnostic/benchmark processでも未定義動作・crashになり得る。production default buildは `--enable-grpc-bench` を含まないため直接影響しないが、このrepositoryが維持しているbench/diagnostic build surfaceは壊れる。また現行 `check-c-sanitizer.sh` は `--enable-grpc-bench` を付けないため、この組合せ回帰を検出できない。
- Recommended fix: 最小修正として、diagnostic callerのcore failure分岐ではconnectionがcalleeにより消費・破棄され得る契約に合わせ、`connection_usable()` / `remove_unusable_persistent_connection()` を呼ばない。より広い修正なら、core APIにconnection lifetime outcomeを明示して全callerで一貫させる。`--enable-grpc-bench --enable-grpc-test-fault` のASan regressionを追加し、fault 1回が期待するexceptionだけで終了してsanitizer reportがないことを固定する。
- Fix summary: `pending`
- Fix commit: `pending`
- Verification: `--enable-grpc --enable-grpc-bench --enable-grpc-test-fault` のASan/UBSan buildによる上記1-call probeで再現。既存のASan/UBSan PHPT 024 / 038 / 039は3/3 PASSしたため、既存oracleではこのdiagnostic caller pathを通らず検出できないことも確認。標準 `./tools/test/check-phpt.sh` は24/24 PASS。
- Notes: `3e128d4` がunary fatal branchへ追加した即時destroyにより顕在化した。production wrapper (`src/wrapper_adapter.c:516-518`) はcore failure時に即returnしてpointerを再参照しない。

## Review Result

- Blocker: none
- High: 1 (Open)
- Medium: none
- Low: none
- Design Decision: none

## Re-verification Notes

- fault seam boundary: `config.m4` の `--enable-grpc-test-fault` はdefault offで、`transport.c` の実装とMINIT初期化は `PHP_GRPC_LITE_ENABLE_TEST_FAULT` guard内、production側のcall siteはconstant `false` macroになる。PHPT / C coverage / sanitizer / ZTS test buildだけが明示的にflagを付ける構成へ変更されている。
- fault snapshot oracle: PHPT 038はMINIT snapshot後の `putenv()` 変更がseamへ影響しないことと、`submit-request-fatal-decoy` がexact tokenとしてsubmit faultを発火しないことを固定している。
- partial-message oracle: PHPT 024はfixture `:50057` に対しunary / server streaming双方のexact `UNAVAILABLE` をassertする。
- cache eviction oracle: PHPT 039は130個の異なる `grpc.default_authority` を使い、各attemptがcache exhaustionではなく `nghttp2_submit_request failed` になること、およびwire prefaceが合計133件であることをassertする。
- ASan/UBSan targeted result: PHPT 024 / 038 / 039は3/3 PASS、sanitizer reportなし。ただし上記Highは同じbuildに `--enable-grpc-bench` を加えたdiagnostic entrypointで再現する。
- Standard NTS PHPT result: `./tools/test/check-phpt.sh` は24/24 PASS（skip / warn / failなし）。
