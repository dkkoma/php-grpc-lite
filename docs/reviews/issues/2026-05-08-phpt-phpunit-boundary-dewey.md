# PHPT PHPUnit boundary Dewey review 2026-05-08

## Scope

- `ext/grpc/tests/*.phpt`
- `ext/grpc/tests/helpers.inc`
- `tests/Integration/*.php`
- `tests/Integration/Spanner/*.php`
- `bench/check-native-phpt.sh`
- `docs/guides/code-reading-guide.md`
- `docs/reviews/issues/2026-05-08-phpt-phpunit-boundary-dewey.md`
- `docs/reviews/issues/2026-05-08-phpt-phpunit-boundary-self-review.md`

## Reviewer Role

- `PHP extension test architecture reviewer`

## Review Prompt Summary

- Re-review after fixes whether migrated PHPT coverage has the right boundary with PHPUnit, whether remaining PHPUnit tests cover higher-level integration and compatibility cases, whether PHPT gate semantics are deterministic for CI, and whether docs accurately describe the split.

## Issues

### REVIEW-20260508-DEWEY-PHPT-PHPUNIT-001: Migrated protocol smoke cases still have exact PHPUnit duplicates

- Severity: `Medium`
- Status: `Fixed`
- Reviewer role: `PHP extension test architecture reviewer`
- Finding: Several low-level protocol/error smoke cases now covered by PHPT remain as exact PHPUnit tests instead of being reduced to higher-level variants.
- Evidence: `ext/grpc/tests/022-error-and-http-validation.phpt`, `tests/Integration/CompressionTest.php`, `tests/Integration/HttpValidationTest.php`
- Expected model: Once a deterministic PHPT gate owns C-extension protocol smoke coverage, PHPUnit should keep broader integration, recovery, limit, wrapper, or variant coverage rather than reasserting the same one-step extension outcomes.
- Why it matters: The duplicated assertions blur the PHPT/PHPUnit responsibility boundary, increase integration-suite cost, and make future protocol expectation changes require synchronized edits in two gates.
- Recommended fix: Remove the exact PHPUnit duplicates already represented in `022-error-and-http-validation.phpt` and keep only PHPUnit cases that add distinct value, such as server-streaming compression variants, recovery after stream-local failures, grpc-status leading-zero validation, and multi-RPC compatibility behavior.
- Fix summary: direct baseline duplicateを追加削減し、`CompressionTest.php` / `HttpValidationTest.php` / `MetadataCompatibilityTest.php` はPHPT baselineにないvariant、recovery、limit、server-streaming互換に絞った。
- Fix commit: `this commit`
- Verification: `./bench/check-native-phpt.sh`; `docker compose run --rm dev sh -lc 'cd ext/grpc && make -j$(nproc) >/tmp/grpc-make.log && cd /workspace && php -d extension=/workspace/ext/grpc/modules/grpc.so vendor/bin/phpunit'`; manual comparison of `022-error-and-http-validation.phpt` with current `CompressionTest.php` / `HttpValidationTest.php`
- Notes: This does not apply to PHPUnit cases that intentionally exercise multi-call recovery, Spanner/wrapper behavior, or larger compatibility matrices.

### REVIEW-20260508-DEWEY-PHPT-PHPUNIT-002: Test documentation still references removed PHPUnit slices

- Severity: `Low`
- Status: `Fixed`
- Reviewer role: `PHP extension test architecture reviewer`
- Finding: The code-reading guide lists PHPUnit files that no longer exist after PHPT migration and still presents some migrated low-level slices as PHPUnit responsibilities.
- Evidence: `docs/guides/code-reading-guide.md`
- Expected model: Test documentation should describe the current split: PHPT owns extension surface and deterministic transport/protocol smoke; PHPUnit owns higher-level integration, wrapper/interceptor behavior, Spanner emulator, complex control semantics, and compatibility/limit variants.
- Why it matters: Stale test names and overlapping responsibility descriptions make the release gate harder to understand and can cause future reviewers to restore duplicate PHPUnit coverage or look for deleted files.
- Recommended fix: Update the test table to remove deleted `UnaryTest.php`, `ServerStreamingTest.php`, and `MetadataControlTest.php` entries, and describe remaining PHPUnit files by the distinct higher-level behavior they still own after PHPT migration.
- Fix summary: direct baseline duplicateを追加削減し、`CompressionTest.php` / `HttpValidationTest.php` / `MetadataCompatibilityTest.php` はPHPT baselineにないvariant、recovery、limit、server-streaming互換に絞った。
- Fix commit: `this commit`
- Verification: review of `docs/guides/code-reading-guide.md` against current `tests/Integration` inventory
- Notes: `README.md` and `AGENTS.md` accurately state the high-level PHPT/PHPUnit command split.

## Review Result

- Blocker: `none`
- High: `none`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
