# PHPT / PHPUnit boundary self-review 2026-05-08

## Scope

- `ext/grpc/tests/*.phpt`
- `ext/grpc/tests/helpers.inc`
- `tests/Integration/*.php`
- `tests/Integration/Spanner/*.php`
- `bench/check-native-phpt.sh`
- `AGENTS.md`
- `README.md`
- `docs/guides/code-reading-guide.md`

## Reviewer Role

- Parent-agent self review
- PHP extension test architecture reviewer

## Review Prompt Summary

- PHPTへ移植したcoverageがPHPUnit側に重複して残っていないかを確認する。
- PHPTとPHPUnitの責務がMECEになっているかを確認する。
- ここでのcoverage 100%は、実測code coverageではなく、現在定義しているrelease/development gate上のテスト責務が漏れなく割り当てられていることを指す。

## Coverage / Responsibility Matrix

| Test responsibility | Primary gate | Representative files | PHPUnit duplicate policy |
| --- | --- | --- | --- |
| Extension load, constants, production public surface | PHPT | `001-load.phpt` | PHPUnit duplicate removed |
| INI defaults and runtime read-ahead INI mutation | PHPT | `002-ini.phpt` | PHPUnit duplicate removed |
| `Grpc\Timeval` arithmetic and compare semantics | PHPT | `003-timeval.phpt` | PHPUnit duplicate removed |
| C object lifecycle, clone rejection, double construction, uninitialized use, channel close surface | PHPT | `004-object-lifecycle.phpt` | PHPUnit duplicate removed |
| Basic unary success, metadata, trailers, sequential channel reuse | PHPT | `010-unary.phpt` | PHPUnit duplicate removed |
| Basic server streaming success and final status | PHPT | `011-server-streaming.phpt` | PHPUnit duplicate removed |
| Request metadata filtering and validation | PHPT | `020-request-metadata-control.phpt` | PHPUnit duplicate removed |
| Deadline status mapping for unary and server streaming | PHPT for status; PHPUnit for timing/count edge behavior | `021-deadline.phpt`, `DeadlineTest.php` | PHPUnit keeps elapsed/count/immediate-deadline integration cases |
| HTTP/gRPC validation, malformed frame, unsupported compression baseline | PHPT for baseline; PHPUnit for additional edge variants and stream-local recovery | `022-error-and-http-validation.phpt`, `HttpValidationTest.php`, `CompressionTest.php` | Direct baseline duplicates removed |
| Binary metadata, duplicate metadata, call credentials baseline | PHPT for baseline; PHPUnit for duplicate binary, large/many metadata, response metadata limit, server streaming duplicate metadata | `023-metadata-and-call-credentials.phpt`, `MetadataCompatibilityTest.php` | Direct unary ASCII/binary/call-credential duplicates removed |
| TLS/mTLS happy path and bad root baseline | PHPT | `030-tls.phpt` | PHPUnit duplicate unary TLS/mTLS cases removed |
| TLS server streaming and mTLS missing-client-cert negative path | PHPUnit | `TlsTest.php`, `MtlsTest.php` | Kept as integration-only variants not covered by PHPT |
| Cancellation, RST_STREAM, GOAWAY, abandoned stream lifecycle, read-ahead bound | PHPUnit | `ControlSemanticsTest.php` | Kept as complex integration/lifecycle semantics |
| Interceptor wrapper behavior | PHPUnit | `InterceptorTest.php` | Kept as PHP wrapper behavior |
| Spanner emulator compatibility | PHPUnit | `tests/Integration/Spanner/*.php` | Kept as application-level integration |

## Issues

### REVIEW-20260508-SELF-001: PHPT migration left duplicated PHPUnit cases

- Severity: `High`
- Status: `Fixed`
- Reviewer role: `Parent-agent self review`, `PHP extension test architecture reviewer`
- Finding: PHPTを追加した後も、Timeval、extension surface、basic unary/server streaming、metadata validation、binary metadata、call credentials、TLS/mTLS baselineなどの同一責務がPHPUnit側に残っていた。
- Evidence: removed `tests/Integration/BinaryMetadataTest.php`, `tests/Integration/CallCredentialsTest.php`, `tests/Integration/ExtensionSurfaceTest.php`, `tests/Integration/MetadataControlTest.php`, `tests/Integration/ServerStreamingTest.php`, `tests/Integration/TimevalTest.php`, `tests/Integration/UnaryTest.php`; trimmed `TlsTest.php`, `MtlsTest.php`, `HttpValidationTest.php`, `CompressionTest.php`, `MetadataCompatibilityTest.php`
- Expected model: C extension surfaceとbaseline transport smokeはPHPTをprimary gateにし、PHPUnitは高レベル統合、複雑な制御系、追加互換ケース、Spanner wrapper検証に寄せる。
- Why it matters: PHPT主力化後にPHPUnit重複を残すと、CIコストとメンテナンス負荷が増え、どちらが一次責務か不明確になる。
- Recommended fix: PHPTと同一責務のPHPUnitを削除し、PHPUnitに残すケースはPHPTで扱わない追加variantだけにする。
- Fix summary: 直接重複するPHPUnit 7ファイルを削除し、TLS/mTLS/HTTP validation/compression/metadata compatibilityの重複メソッドを削除した。PHPUnitは79 testsから36 testsへ減った。
- Fix commit: `this commit`
- Verification: `./bench/check-native-phpt.sh`; `docker compose run --rm dev sh -lc 'cd ext/grpc && make -j$(nproc) >/tmp/grpc-make.log && cd /workspace && php -d extension=/workspace/ext/grpc/modules/grpc.so vendor/bin/phpunit'`
- Notes: PHPUnit側に残したdeadline、compression、metadata compatibilityはPHPT baselineとは別のedge/integration責務として扱う。

## Review Result

- Blocker: `none`
- High: `1 fixed`
- Medium: `none`
- Low: `none`
- Design Decision: `none`
