# PHPT test gate review 2026-05-08

## Scope

- `ext/grpc/tests/*.phpt`
- `ext/grpc/tests/helpers.inc`
- `bench/check-native-phpt.sh`
- `AGENTS.md`
- `README.md`
- `docs/guides/code-reading-guide.md`

## Reviewer Role

- C/PHP extension PHPT test reviewer
- CI/test architecture reviewer for PHP C extension

## Review Prompt Summary

- PHPTがC拡張surface / integration smokeとして妥当かを確認する。
- CIの最初のbuilding blockとして、run-tests invocation、skip条件、Composer/vendor前提、ネットワーク依存、失敗artifact、PHPT/PHPUnitの責務分離を確認する。
- PHPT側で可能な範囲のカバレッジ不足を、分解点として妥当な単位で指摘する。

## Issues

### REVIEW-20260508-PHPT-001: PHPT runner uses `sh` with `pipefail`

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `C/PHP extension PHPT test reviewer`, `CI/test architecture reviewer for PHP C extension`
- Finding: `bench/check-native-phpt.sh` が `sh -lc` の内側で `set -euo pipefail` を使っている。Debian `/bin/sh` は通常 `dash` であり、`pipefail` はportableではない。
- Evidence: `bench/check-native-phpt.sh`
- Expected model: PHPT check commandは、宣言済みDocker dev imageで決定的に動く。
- Why it matters: CI/local verificationがbuild/test前にshell optionで失敗し、PHPT gateとして使えなくなる。
- Recommended fix: container内の実行shellを `bash -lc` にするか、`pipefail` を外す。
- Fix summary: `docker compose run --rm dev bash -lc` に変更した。
- Fix commit: `this commit`
- Verification: `./bench/check-native-phpt.sh`
- Notes: 11 PHPTすべてpass。

### REVIEW-20260508-PHPT-002: PHPT helper makes all tests depend on Composer vendor files

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `C/PHP extension PHPT test reviewer`, `CI/test architecture reviewer for PHP C extension`
- Finding: `helpers.inc` が無条件に `vendor/autoload.php` をrequireしており、load / INI / Timevalなどのextension-only PHPTまでfresh checkoutのComposer未実行状態で失敗する。
- Evidence: `ext/grpc/tests/helpers.inc`, `bench/check-native-phpt.sh`
- Expected model: C拡張surface PHPTは最小依存で動き、Composer fixtureが必要なintegration PHPTだけがautoload前提を持つ。
- Why it matters: CIの最初のPHPT gateが、C拡張を検証する前にvendor欠落で失敗する。
- Recommended fix: extension-only helperとintegration helperを分ける、またはautoload requireを必要なPHPTだけに遅延する。runner/docsではComposer前提を明示する。
- Fix summary: `helpers.inc` のautoload requireを `grpc_lite_phpt_require_autoload()` へ遅延し、integration PHPTの `SKIPIF` でautoload存在確認を行う。runnerは `vendor/autoload.php` をpreflightで必須化した。
- Fix commit: `this commit`
- Verification: `./bench/check-native-phpt.sh`
- Notes: extension-only PHPTはautoloadなしでも構文上独立した。

### REVIEW-20260508-PHPT-003: Network-dependent PHPT tests can be silently skipped in the main gate

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `CI/test architecture reviewer for PHP C extension`
- Finding: unary / server streaming / metadata / deadline / TLS PHPT は `test-server` が到達不能だとskipする一方、main runnerは全PHPTを1つのgateとして扱う。
- Evidence: `ext/grpc/tests/010-unary.phpt`, `ext/grpc/tests/011-server-streaming.phpt`, `ext/grpc/tests/020-request-metadata-control.phpt`, `ext/grpc/tests/021-deadline.phpt`, `ext/grpc/tests/030-tls.phpt`, `bench/check-native-phpt.sh`
- Expected model: main PHPT gateで必要なserviceがない場合は、silent skipではなくpreflight failureにする。
- Why it matters: CIがgreenでも、transport系PHPTがskipされてcoverageが落ちる可能性がある。
- Recommended fix: smoke/integration directoryを分けるか、required-service modeでrunnerがrun-tests前に到達性を確認する。
- Fix summary: `check-native-phpt.sh` で `test-server:50051/50052/50053/50054` のpreflightを追加し、runner経由ではservice欠落をfailureにする。PHPT単体実行時のために各PHPTのskipは残す。
- Fix commit: `this commit`
- Verification: `./bench/check-native-phpt.sh`
- Notes: runnerでは11 PHPTすべてpass、skip 0。

### REVIEW-20260508-PHPT-004: Server streaming PHPT uses tight wall-clock timing thresholds

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `C/PHP extension PHPT test reviewer`
- Finding: `011-server-streaming.phpt` が最初のyield `<50ms`、後続gap `70ms..150ms` をassertしており、CI schedulerに弱い。
- Evidence: `ext/grpc/tests/011-server-streaming.phpt`
- Expected model: PHPTはCIで安定する決定的な期待値を持つ。細かいpacing観測はPHPUnit integrationまたはbench diagnosticに置く。
- Why it matters: Docker/CI負荷で正しい実装でもflaky failureになる。
- Recommended fix: PHPTはmessage order/statusのsmokeにし、incremental pacingはPHPUnit/benchへ残す。
- Fix summary: PHPTからwall-clock timing assertionを削除し、server streamingのmessage listとstatus検証に絞った。
- Fix commit: `this commit`
- Verification: `./bench/check-native-phpt.sh`
- Notes: PHPUnit側のincremental yield testは残している。

### REVIEW-20260508-PHPT-005: TLS PHPT skip checks only one certificate fixture

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `C/PHP extension PHPT test reviewer`
- Finding: `030-tls.phpt` は `server.crt` のみをskip条件で確認していたが、FILE sectionでは `client.crt` と `client.key` も使う。
- Evidence: `ext/grpc/tests/030-tls.phpt`
- Expected model: PHPT `SKIPIF` はFILE sectionで必要な外部fixtureをすべて確認する。
- Why it matters: 部分的にfixtureが欠けた環境で、skipではなくtest failureになる。
- Recommended fix: `server.crt`, `client.crt`, `client.key` をすべて確認する。
- Fix summary: `SKIPIF` で3つのcert fixtureを確認するようにした。
- Fix commit: `this commit`
- Verification: `./bench/check-native-phpt.sh`

### REVIEW-20260508-PHPT-006: Exception helper can hide wrong failure class for surface tests

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `C/PHP extension PHPT test reviewer`
- Finding: `grpc_lite_phpt_expect_throw()` が任意の `Throwable` を受け入れるため、surface testで誤った例外classを隠す可能性がある。
- Evidence: `ext/grpc/tests/helpers.inc`, `ext/grpc/tests/004-object-lifecycle.phpt`
- Expected model: extension surface testは、既知のAPI misuseについて期待する例外classまたはmessageを検証できる。
- Why it matters: unrelated autoload/reflection/userland errorでtestが誤ってpassする可能性がある。
- Recommended fix: helperに期待class引数を追加し、分かる箇所で使う。
- Fix summary: `grpc_lite_phpt_expect_throw()` に `$expectedClass` を追加し、clone禁止の検証で `Error::class` をassertした。
- Fix commit: `this commit`
- Verification: `./bench/check-native-phpt.sh`

### REVIEW-20260508-PHPT-007: PHPT failure artifacts are not ignored or isolated

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `CI/test architecture reviewer for PHP C extension`
- Finding: PHPTをin-place実行すると、失敗時に `.out`, `.diff`, `.exp`, `.log`, generated `.php`, `.sh` が `ext/grpc/tests/` に残る。
- Evidence: `bench/check-native-phpt.sh`, `.gitignore`
- Expected model: test artifactはignoredにするか、runnerが掃除する。
- Why it matters: 失敗後のworktreeが汚れ、review diffがノイズになる。
- Recommended fix: `ext/grpc/tests/` 配下のPHPT artifact ignore patternを追加し、runnerで成功時に掃除する。
- Fix summary: `.gitignore` にPHPT artifact patternを追加し、runnerの前後でartifactを削除するようにした。
- Fix commit: `this commit`
- Verification: `./bench/check-native-phpt.sh`

### REVIEW-20260508-PHPT-008: Docs do not clearly separate PHPT and PHPUnit responsibilities

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `CI/test architecture reviewer for PHP C extension`
- Finding: docsはPHPT/PHPUnitコマンドを列挙しているが、PHPTの役割、必要サービス、skip/fail policyが不明確。
- Evidence: `README.md`, `docs/guides/code-reading-guide.md`, `AGENTS.md`
- Expected model: PHPTはC拡張surface + transport smoke、PHPUnitは広いintegration/release compatibilityとして位置づける。
- Why it matters: CI設計者がPHPTだけで全integration coverageと誤解する可能性がある。
- Recommended fix: PHPTカテゴリ、必要service、CI順序、PHPUnitとの責務分離をdocsへ明記する。
- Fix summary: `AGENTS.md`, `README.md`, `docs/guides/code-reading-guide.md` にPHPT gateとPHPUnit integrationの位置づけを追記した。
- Fix commit: `this commit`
- Verification: document review

### REVIEW-20260508-PHPT-009: PHPT coverage lacks protocol error and metadata/call-credentials slices

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `C/PHP extension PHPT coverage reviewer`
- Finding: 初期PHPTはnormal unary/server streaming、metadata validation、deadline、TLS/mTLSを押さえていたが、gRPC status/error mapping、malformed frame、compression unsupported、binary/duplicate metadata、call credentialsのC bridge境界がPHPT側に無かった。
- Evidence: `ext/grpc/tests/*.phpt`, `tests/Integration/CompressionTest.php`, `tests/Integration/HttpValidationTest.php`, `tests/Integration/ErrorSemanticsTest.php`, `tests/Integration/BinaryMetadataTest.php`, `tests/Integration/CallCredentialsTest.php`
- Expected model: PHPT主力化では、C拡張の分解点として妥当なprotocol error、metadata ownership、call credentials bridgeを小さなPHPTに分けて持つ。
- Why it matters: PHPUnit integrationだけに残すと、C拡張変更時の低レベルregression gateとしてのPHPT coverageが弱い。
- Recommended fix: protocol error/HTTP validation/compressionを1 PHPT、metadata/call-credentialsを1 PHPTとして追加する。
- Fix summary: `022-error-and-http-validation.phpt` と `023-metadata-and-call-credentials.phpt` を追加した。
- Fix commit: `this commit`
- Verification: `./bench/check-native-phpt.sh`

### REVIEW-20260508-PHPT-010: PHPT service preflight has no readiness retry

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `CI/test architecture reviewer for PHP C extension`
- Finding: `check-native-phpt.sh` は Go test-server ports `50051` / `50052` / `50053` / `50054` をpreflightで必須化したが、各portを1回だけ `fsockopen(..., 1.0)` で確認している。`docker compose run` の `depends_on` はservice起動順だけを保証し、test-server readinessは保証しない。
- Evidence: `bench/check-native-phpt.sh`, `compose.yaml`
- Expected model: CIの最初のPHPT gateは、required serviceが起動中の短いraceでflaky failureにならない。service欠落はfailure、起動待ちはbounded retryで吸収する。
- Why it matters: GitHub Actionsなどでimage build直後にPHPT runnerを起動すると、test-serverがまだlistenしていない瞬間にpreflightが失敗し、実装と無関係なCI failureになる可能性がある。
- Recommended fix: runner側で各portに対して短いretry/backoff付きwait loopを入れる、または `test-server` にhealthcheckを追加してCIがhealthyを待ってからPHPTを実行する。
- Fix summary: `check-native-phpt.sh` のservice preflightを各port最大30回、100ms間隔のbounded retryにした。service欠落はfailureのまま、起動直後の短いreadiness raceだけ吸収する。
- Fix commit: `this commit`
- Verification: `./bench/check-native-phpt.sh`

## Review Result

- Blocker: `none`
- High: `2 fixed`
- Medium: `5 fixed`
- Low: `3 fixed`
- Design Decision: `none`
