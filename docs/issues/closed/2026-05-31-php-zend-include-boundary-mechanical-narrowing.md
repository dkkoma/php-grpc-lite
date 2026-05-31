# PHP/Zend include boundary: mechanical include narrowing

- Status: Closed
- Created: 2026-05-31
- Branch: codex/php-zend-mechanical-narrowing
- Owner: Codex
- Parent: docs/issues/closed/2026-05-31-php-zend-include-boundary.md
- Related-Design: docs/design/php-zend-include-boundary.md

## Background

`src/common.h` はPHP/Zend、nghttp2、OpenSSL、socket/system headerをまとめて読むprivate convenience headerである。PHP/Zend boundaryに近いheaderが `common.h` を読むこと自体は許容できるが、OpenSSL helperやresult bridgeなどが不要なnghttp2/socket依存まで受け取っている。

このissueでは、挙動、struct layout、function boundaryを変えず、header includeだけを必要なものへ薄くする。

## Goals

- `common.h` から離せるheaderを、直接必要なincludeへ置き換える。
- PHP/Zend依存が必要なheaderは `php.h` を明示的に読む。
- OpenSSLだけが必要なheaderはOpenSSLとC standard headerだけを読む。
- nghttp2/socket/system依存を不要なheaderへ広げない。

## Candidate Scope

最初の候補:

- `src/tls_config.h`
  - `common.h` ではなく、`<stddef.h>` とOpenSSL型を提供するheaderを直接読む。
- `src/grpc_result.h`
  - `zend_string` / `zval` / `bool` が必要なのでPHP-awareでよいが、`common.h` 経由でnghttp2/OpenSSL/socketまで読む必要はない。
- `src/wrapper_adapter.h`
  - `PHP_METHOD` のためPHP/Zend依存は自然だが、`common.h` 全体は不要。
- `src/diagnostic/bench_call.h`
  - bench-only fieldで `zend_long` とC scalarが必要。`common.h` 全体を読む必要があるか確認する。

## Non-Goals

- 関数signatureを変えない。
- `grpc_call` / `h2_connection` / diagnostic structのfield orderを変えない。
- allocation、metadata conversion、TLS設定処理を変えない。
- `common.h` 自体の大規模分割はこのissueでは行わない。
- PHP/Zend依存が必要なheaderを機械的にすべて `common.h` へ寄せない。

## Verification

- `git diff --check`
- `docker compose run --rm dev sh -lc 'make -j$(nproc)'`
- `./tools/test/check-c-static-analysis.sh`
- `./tools/test/check-c-unit.sh`
- `./tools/test/check-phpt.sh`

runtime挙動変更なしのためbenchmarkは不要。

## Decision Log

- 2026-05-31: まずは挙動変更なしのinclude narrowingとして扱う。PHP/Zend依存が必要なheaderから `php.h` を消すことは目的にしない。
- 2026-05-31: 対象は `tls_config.h`、`grpc_result.h`、`wrapper_adapter.h`、`diagnostic/bench_call.h` に限定した。signature、struct layout、function boundary、allocation policyは変更しない。
- 2026-05-31: `tls_config.h` はOpenSSL型と `size_t` だけを公開するため、`common.h` を外して `<stddef.h>` と `<openssl/ssl.h>` を直接読む形にした。`tls_config.c` は `INT_MAX`、`true`、OpenSSL PEM/error helperを使うため、必要なincludeを `.c` 側へ移した。
- 2026-05-31: `grpc_result.h` と `wrapper_adapter.h` はPHP-aware headerとして残すが、`common.h` 全体ではなく `<php.h>` と必要なC standard headerだけを読む形にした。
- 2026-05-31: `diagnostic/bench_call.h` はbench-onlyのPHP-aware headerとして `<php.h>`、`<stdbool.h>`、`<stddef.h>`、`<stdint.h>` を直接読む形にした。

## Progress

- 2026-05-31: `src/tls_config.h`、`src/grpc_result.h`、`src/wrapper_adapter.h`、`src/diagnostic/bench_call.h` から `common.h` 依存を削除。
- 2026-05-31: `src/tls_config.c` にTLS helper実装で必要なOpenSSL/C standard includeを追加。

## Verification Results

- `git diff --check`: pass
- `docker compose run --rm dev sh -lc 'make -j$(nproc)'`: pass
- `./tools/test/check-c-static-analysis.sh`: pass
- `./tools/test/check-c-unit.sh`: pass
- `./tools/test/check-phpt.sh`: pass, 15/15
- Benchmark: not run。include narrowingのみでruntime挙動、signature、layout、function boundaryを変更していないため不要。

## Close Criteria

- 対象headerから不要な `common.h` 依存が減っている、または減らさない理由が記録されている。
- build/static analysis/C unit/PHPTが通っている。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
