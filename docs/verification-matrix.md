# Verification matrix

このmatrixは、HTTP/2 / gRPC semanticsをどのverification layerが守っているかを読むための入口である。詳細なレビュー観点は `docs/protocol-model-review-guide.md`、fixtureの意味は `docs/test-fixtures.md` を参照する。

Legend:

- `C unit`: `tests/unit/*.c`
- `Fuzz`: `tests/fuzz/*`
- `PHPT`: `tests/phpt/*.phpt`
- `PHPUnit`: `tests/Integration/**/*.php`
- `Hardening`: sanitizer / Valgrind / lifecycle / ZTS / release script

## Behavior coverage

| semantics | unary | server streaming | Main verification |
|---|---|---|---|
| extension load、定数、production diagnostic boundary | covered | covered | `tests/phpt/001-load.phpt` |
| INI defaultとHTTP/2 setting入力 | covered | covered | `tests/phpt/002-ini.phpt`, `tests/unit/test_transport_core.c` |
| Timeval / deadline value object | covered | covered | `tests/phpt/003-timeval.phpt` |
| PHP object lifecycleと誤用 | covered | covered | `tests/phpt/004-object-lifecycle.phpt` |
| Basic h2c success path | `tests/phpt/010-unary.phpt` | `tests/phpt/011-server-streaming.phpt` | PHPT, `tests/Integration/ControlSemanticsTest.php` |
| Request metadata validation | covered | covered | `tests/phpt/020-request-metadata-control.phpt` |
| Duplicate ASCII/binary metadata preservation | covered | covered | `tests/Integration/MetadataCompatibilityTest.php` |
| Response metadata size limit | covered | covered | `tests/phpt/025-resource-limits.phpt`, `tests/Integration/MetadataCompatibilityTest.php` |
| Deadline enforced client-side | covered | covered | `tests/phpt/021-deadline.phpt`, `tests/Integration/DeadlineTest.php` |
| HTTP status fallback without grpc-status | covered | covered | `tests/phpt/022-error-and-http-validation.phpt` |
| Invalid / non-gRPC content-type | covered | covered | `tests/phpt/022-error-and-http-validation.phpt`, `tests/Integration/HttpValidationTest.php` |
| Invalid grpc-status value | covered | covered | `tests/phpt/022-error-and-http-validation.phpt`, `tests/Integration/HttpValidationTest.php`, `tests/unit/test_protocol_core.c` |
| Compressed response flag / unsupported encoding | covered | covered | `tests/phpt/022-error-and-http-validation.phpt`, `tests/Integration/CompressionTest.php` |
| Malformed / partial gRPC frame | covered | thin | `tests/phpt/022-error-and-http-validation.phpt`, `tests/phpt/024-control-semantics.phpt` |
| Call credentials metadata merge | covered | covered | `tests/phpt/023-metadata-and-call-credentials.phpt` |
| Insecure channel rejects call credentials | covered | covered | `tests/phpt/004-object-lifecycle.phpt`, `tests/phpt/023-metadata-and-call-credentials.phpt` |
| Client cancel | covered | covered | `tests/phpt/024-control-semantics.phpt`, `tests/Integration/ControlSemanticsTest.php` |
| Dropped server stream does not poison connection | n/a | covered | `tests/Integration/ControlSemanticsTest.php` |
| Server-sent RST_STREAM / REFUSED_STREAM recovery | covered | covered | `tests/phpt/024-control-semantics.phpt` |
| GOAWAY after successful stream | covered | thin | `tests/phpt/024-control-semantics.phpt` |
| GOAWAY refusing new stream | covered | thin | `tests/phpt/024-control-semantics.phpt` |
| EOF / socket close and recovery | covered | thin | `tests/phpt/024-control-semantics.phpt` |
| TLS success | covered | covered | `tests/phpt/030-tls.phpt`, `tests/Integration/TlsTest.php` |
| mTLS success / missing client cert failure | covered | not primary | `tests/phpt/030-tls.phpt`, `tests/Integration/MtlsTest.php` |
| Bad root / TLS verification failure | covered | not primary | `tests/phpt/030-tls.phpt` |
| OpenTelemetry trace context metadata support | wrapper-level | wrapper-level | `tests/phpt/027-otel-trace-context-metadata.phpt` |
| Trace file emission | covered | covered | `tests/phpt/029-trace-file.phpt` |
| Spanner emulator compatibility | covered | covered | `tests/Integration/Spanner/*` |
| pure protocol boundary value | parser only | parser only | `tests/unit/test_protocol_core.c`, `tests/fuzz/fuzz_protocol_core.c` |
| status taxonomy priority | status only | status only | `tests/unit/test_status_core.c` |
| transport入力limit / authority validation | input only | input only | `tests/unit/test_transport_core.c` |
| ZTS build/load/PHPT | covered | covered | `.github/workflows/native-qa.yml`, `tools/test/check-zts-phpt.sh` |

## Thin or intentionally separate coverage

| Area | Current status | Follow-up owner |
|---|---|---|
| Server-streaming GOAWAY / EOF matrix | `024-control-semantics` で間接的にcovered。ただしunaryほど明示的ではない | `docs/issues/open/2026-05-31-exemplar-test-discoverability-and-gates.md` |
| DNS resolution中のdeadline | 現在のconnect/read/write/TLS deadline挙動はcovered。blocking resolverの挙動は既知の設計上の注意点として、別途document化または再設計する | future transport behavior issue |
| TLS hostname mismatch / authority override end-to-end | input validationはあるが、end-to-endのpositive/negative matrixは薄い | future TLS compatibility issue |
| Slow-consumer memory gate | diagnostic scriptはあるが、gateかmeasurement-onlyかの扱いを明確にする必要がある | `docs/issues/open/2026-05-31-exemplar-test-discoverability-and-gates.md` |
| Lifecycle FD/RSS thresholds | release scriptはlifecycle checkを実行するが、threshold enforcementの扱いを明確にする必要がある | `docs/issues/open/2026-05-31-exemplar-test-discoverability-and-gates.md` |
| `grpc_call` field ownership | behavior testではcoveredだが、field mapとしては未文書化 | `docs/issues/open/2026-05-31-exemplar-grpc-call-exchange-state-map.md` |
| Protocol classification vs transport action | behaviorとしてはcoveredだが、responsibility boundaryがまだ見えづらい | `docs/issues/open/2026-05-31-exemplar-protocol-classification-boundary.md` |

## Gate mapping

| gate | command | protects |
|---|---|---|
| development gate | `./tools/test/check-native-development-gate.sh` | C static analysis、C unit、PHPT、短時間fuzz smoke |
| C unit | `./tools/test/check-c-unit.sh` | pure protocol/status/transport helper boundary |
| PHPT | `./tools/test/check-phpt.sh` | extension load、PHP-visible low-level surface、local transport behavior |
| C fuzz smoke | `./tools/test/check-c-fuzz.sh` | 短時間のdeterministic fuzz runでprotocol helperのrobustnessを見る |
| C coverage | `./tools/test/check-c-coverage.sh` | C unit + PHPTによるC line/function coverage |
| PHPUnit integration | `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit` | wrapper/API compatibilityとSpanner emulator path |
| Release hardening | `./tools/test/check-native-release-hardening.sh` | sanitizer、Valgrind、lifecycle、FPM request-boundary hardening |
| ZTS PHPT | `./tools/test/check-zts-phpt.sh` | ZTS build/load/PHPT compatibility |
