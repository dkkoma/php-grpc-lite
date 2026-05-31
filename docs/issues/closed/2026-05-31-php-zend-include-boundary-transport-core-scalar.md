# PHP/Zend include boundary: transport_core scalar boundary

- Status: Closed
- Created: 2026-05-31
- Branch: codex/php-zend-transport-core-scalar
- Owner: Codex
- Parent: docs/issues/closed/2026-05-31-php-zend-include-boundary.md
- Related-Design: docs/design/php-zend-include-boundary.md
- Fix Commit: this commit

## Background

`src/transport_core.h` は「Pure HTTP/2 transport helpers」とコメントされているが、`zend_long` / `zend_ulong` のために `php.h` を読む。実装内容はsize/window/metadata limit、authority construction、target/path validationであり、PHP runtime objectやZend allocationには依存していない。

この層はC/PHP extensionのお手本として、PHP scalar boundaryとpure transport policyの分離を示す価値が高い。

## Goals

- `transport_core.h` をC standard headersだけで読める形へ寄せる。
- PHP INI / user option由来の `zend_long` は、呼び出し側でC scalarへ変換してからtransport policy helperへ渡す。
- C unit / fuzzに近い層がPHP runtime includeなしで読める状態を目指す。

## Investigation Points

- `zend_long` を `int64_t` に置き換えた場合、32-bit PHP buildやZTSで意味が変わらないか。
- `zend_ulong hash_bytes()` を残す必要があるか。
  - 過去にはpersistent connection identity照合に使っていた。
  - 現在はSHA-256 connection keyへ移行済みでproduction用途がない。
- `build_authority()` の `port` はPHP由来だが、transportとしては1..65535のC integerで足りる。
- test側で `zend_long` castを使っている箇所をどう置き換えるか。

## Proposed Approach

1. `transport_core.h` のpublic signatureをC scalarへ置き換える案を作る。
2. `surface.c`、`transport.c`、`server_streaming_call.c`、`diagnostic/bench.c` の呼び出し側でPHP scalarからC scalarへ変換する位置を確認する。
3. C unitで境界値を維持する。
4. PHPTでuser-visible behaviorが変わらないことを確認する。

## Non-Goals

- limit policyやvalidation messageを変えない。
- HTTP/2 window / frame / metadata limitの意味を変えない。
- `grpc_call` layoutやtransport hot pathを変えない。

## Verification

- `git diff --check`
- `./tools/test/check-c-static-analysis.sh`
- `./tools/test/check-c-unit.sh`
- `./tools/test/check-phpt.sh`

function boundaryやallocationを増やさない限りbenchmarkは不要。

## Decision Log

- 2026-05-31: このissueはinclude整理ではなく、PHP scalar boundaryの設計変更として扱う。小さい変更だがC/PHP境界の教材価値は高い。
- 2026-05-31: `transport_core.h` から `php.h` を外し、PHP由来の `zend_long` は呼び出し側で `int64_t` に明示変換してからtransport policy helperへ渡す。64-bit PHPでは実質的に同じ値域を保ち、32-bit PHPではC unitからPHP userlandでは渡れない大きな値も検証できるため、上限飽和とport拒否をC unitで明示する。
- 2026-05-31: `hash_bytes()` は過去にpersistent connection identity照合で使われていたが、現在はSHA-256 connection keyへ移行済みでproduction用途がなく、testからしか参照されていない。`zend_ulong` を `uint64_t` に置き換えて意味を再定義するより、未使用helperとして削除する。
- 2026-05-31: portのPHP boundaryは、現時点では `surface.c`、`diagnostic/bench.c`、`server_streaming_call.c` などのPHP/Zend-aware caller側に残す。`transport_core` は未検証のC scalarを受け取って `validate_channel_inputs()` で `1..65535` を判定するpolicy boundaryであり、validated後のtransport内部を `uint16_t` にする変更はこのissueへ混ぜない。`validate_channel_inputs()` は `-1`、`0`、`65536`、`INT64_MAX` を拒否し、`1`、`65535` を許可する。
- 2026-05-31: function boundaryやallocationは増やしていないため、benchmarkは不要と判断した。挙動維持はC unitとPHPTで確認する。

## Progress

- `src/transport_core.h` をC standard headersだけで読める形にした。
- `src/transport_core.c` に必要な `<inttypes.h>`、`<stdio.h>`、`<string.h>` を直接includeした。
- PHP/Zend-aware caller側で `zend_long` を `int64_t` へ明示変換し、transport policy helperに渡す境界をコード上でも見えるようにした。
- `tests/unit/test_transport_core.c` から `transport.h` 依存を外し、`transport_core.h` 単体のinclude境界を検証できる形にした。
- C unitに `INT64_MAX`、`UINT32_MAX + 1`、port境界を追加した。
- `docs/design/php-zend-include-boundary.md` の `transport_core.h` 現状表を更新した。

## Verification Result

- `git diff --check`: passed
- `docker compose run --rm dev sh -lc 'make -j$(nproc)'`: passed
- `./tools/test/check-c-static-analysis.sh`: passed
- `./tools/test/check-c-unit.sh`: passed
- `./tools/test/check-phpt.sh`: passed, 15/15

## Close Criteria

- `transport_core.h` から `php.h` を外すか、外さない理由が明確に記録されている。
- signature変更する場合はC unitとPHPTが通っている。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
