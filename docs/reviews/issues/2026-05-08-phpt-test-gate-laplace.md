# PHPT test gate review Laplace 2026-05-08

## Scope

- `ext/grpc/tests/*.phpt`
- `ext/grpc/tests/helpers.inc`
- `bench/check-native-phpt.sh`
- `docs/reviews/README.md`
- `docs/reviews/templates/subagent-review-prompt.md`
- `docs/reviews/issues/`
- `AGENTS.md`
- `README.md`
- `docs/guides/code-reading-guide.md`

## Reviewer Role

- C/PHP extension PHPT test reviewer
- Domain-model reviewer for PHPT release/development gate

## Review Prompt Summary

- PHPT gateがこのrepositoryのC extension surface、transport smoke、wrapper bridgeの低レベルregression gateとして妥当かを確認する。
- skip/preflight、run-tests invocation、artifact handling、deterministic CI behavior、extension-vs-wrapper responsibility、review record usabilityを確認する。
- 各review agentが自分専用のissue fileへ記録する運用に従う。

## Issues

### REVIEW-20260508-LAPLACE-001: Source-built extension load failure can still become all-skip

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `C/PHP extension PHPT test reviewer`, `Domain-model reviewer for PHPT release/development gate`
- Finding: `check-native-phpt.sh` は `run-tests.php` に `-d extension=/workspace/ext/grpc/modules/grpc.so` を渡しているが、runner自身ではそのextensionが実際にloadできたことをpreflightしていない。各PHPTの `SKIPIF` は `extension_loaded('grpc')` がfalseならskipするため、module buildは成功したがloadに失敗する状態が、release gate上は全skipとして扱われる可能性が残る。
- Evidence: `bench/check-native-phpt.sh`, `ext/grpc/tests/001-load.phpt`, `ext/grpc/tests/*.phpt`
- Expected model: source-built C extension PHPT gateでは、対象extensionがloadできない状態はskipではなくgate failureである。PHPT単体実行時のportable skipと、標準runnerのrequired-extension failure policyを分ける。
- Why it matters: missing shared library, ABI mismatch, startup failureなどで `grpc.so` がload不能でも、PHPT本体がすべてskipすればCIがgreenになる可能性があり、release/development gateとして最重要の「extensionがロードできる」不変条件を検出できない。
- Recommended fix: `run-tests.php` 実行前に runner で `php -d extension=/workspace/ext/grpc/modules/grpc.so -r 'exit(extension_loaded("grpc") ? 0 : 1);'` のようなpreflightを追加し、失敗時はstderrへstartup/load failure detailを出してexit 1にする。PHPT側の `SKIPIF` は単体実行用に残してよい。
- Fix summary: `check-native-phpt.sh` に `php -d extension=/workspace/ext/grpc/modules/grpc.so -r ...` のload preflightを追加し、source-built extensionがload不能な場合はrun-tests前にfailureにした。
- Fix commit: `this commit`
- Verification: `./bench/check-native-phpt.sh`
- Notes: service preflight、autoload preflight、artifact cleanup、PHPT/PHPUnit responsibility docs、review-record運用は現状のscopeでは妥当。

## Review Result

- Blocker: `none`
- High: `1 fixed`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
