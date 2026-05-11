# PHPT / PHPUnit boundary review Laplace 2026-05-08

## Scope

- `ext/grpc/tests/*.phpt`
- `ext/grpc/tests/helpers.inc`
- `tests/Integration/*.php`
- `bench/check-native-phpt.sh`
- `phpunit.xml.dist`
- `AGENTS.md`
- `README.md`
- `docs/code-reading-guide.md`
- `docs/reviews/README.md`
- `docs/reviews/templates/subagent-review-prompt.md`

## Reviewer Role

- PHP extension test architecture reviewer
- PHPT / PHPUnit boundary reviewer

## Review Prompt Summary

- PHPTへ移したC extension surface / transport smoke coverageと、残すPHPUnit integration coverageの責務境界を確認する。
- PHPT gateのCI determinism、required extension/service preflight、artifact handling、silent skip防止を確認する。
- docsが現在のtest inventoryとPHPT/PHPUnit responsibility splitを正しく説明しているか確認する。

## Issues

### REVIEW-20260508-LAPLACE-001: code-reading guide still references removed PHPUnit suites

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `PHP extension test architecture reviewer`, `PHPT / PHPUnit boundary reviewer`
- Finding: PHPT移行後のtest responsibility split自体は概ね妥当だが、`docs/code-reading-guide.md` の主要テスト表が現在存在しない `tests/Integration/UnaryTest.php`, `tests/Integration/ServerStreamingTest.php`, `tests/Integration/MetadataControlTest.php` をまだ列挙している。これらの基本coverageはPHPTへ移され、現在の `tests/Integration/*.php` には存在しない。
- Evidence: `docs/code-reading-guide.md`, `ext/grpc/tests/010-unary.phpt`, `ext/grpc/tests/011-server-streaming.phpt`, `ext/grpc/tests/020-request-metadata-control.phpt`, `tests/Integration/*.php`
- Expected model: docsは現在のテストinventoryと責務境界を説明する。PHPTはC extension surface / deterministic transport smoke、PHPUnitはwrapper/interceptor、complex control semantics、compatibility/limit matrix、Spanner/gax integrationを担当する、という現在の分担をファイル名込みで正しく示す。
- Why it matters: 開発者やreview agentが存在しないPHPUnit suiteを探したり、PHPTへ移したcoverageがまだPHPUnitに重複して残っていると誤解する可能性がある。test architectureの境界がdocsから読み取りにくくなる。
- Recommended fix: `docs/code-reading-guide.md` の主要テスト表を現在のinventoryへ更新する。削除済みのbasic unary/server streaming/metadata-control PHPUnit行はPHPT行へ統合し、残るPHPUnit行は `CompressionTest`, `HttpValidationTest`, `ErrorSemanticsTest`, `MetadataCompatibilityTest`, `ControlSemanticsTest`, `InterceptorTest`, `TlsTest` / `MtlsTest`, `Spanner/*` のように、PHPTより高いintegration/compatibility責務として説明する。
- Fix summary: `docs/code-reading-guide.md` の主要テスト表から削除済みPHPUnit suiteを外し、PHPTと残存PHPUnitの現在の責務境界へ更新した。
- Fix commit: `this commit`
- Verification: `./bench/check-native-phpt.sh`; `docker compose run --rm dev sh -lc 'cd ext/grpc && make -j$(nproc) >/tmp/grpc-make.log && cd /workspace && php -d extension=/workspace/ext/grpc/modules/grpc.so vendor/bin/phpunit'`; review of `docs/code-reading-guide.md` against current test inventory
- Notes: PHPT runnerはrequired extension load、autoload、Go test-server ports、artifact cleanupをpreflightしており、標準runnerとしてsilent skipを避ける構造になっている。残存PHPUnitの重複は、compatibility matrixやcontrol semanticsなどPHPTに寄せすぎない高レベルcoverageとして許容できる。

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
