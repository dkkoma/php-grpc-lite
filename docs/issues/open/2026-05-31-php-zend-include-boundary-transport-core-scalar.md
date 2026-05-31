# PHP/Zend include boundary: transport_core scalar boundary

- Status: Open
- Created: 2026-05-31
- Branch: TBD
- Owner: Codex
- Parent: docs/issues/open/2026-05-31-php-zend-include-boundary.md
- Related-Design: docs/design/php-zend-include-boundary.md

## Background

`src/transport_core.h` は「Pure HTTP/2 transport helpers」とコメントされているが、`zend_long` / `zend_ulong` のために `php.h` を読む。実装内容はsize/window/metadata limit、authority construction、target/path validationであり、PHP runtime objectやZend allocationには依存していない。

この層はC/PHP extensionのお手本として、PHP scalar boundaryとpure transport policyの分離を示す価値が高い。

## Goals

- `transport_core.h` をC standard headersだけで読める形へ寄せる。
- PHP INI / user option由来の `zend_long` は、呼び出し側でC scalarへ変換してからtransport policy helperへ渡す。
- C unit / fuzzに近い層がPHP runtime includeなしで読める状態を目指す。

## Investigation Points

- `zend_long` を `int64_t` に置き換えた場合、32-bit PHP buildやZTSで意味が変わらないか。
- `zend_ulong hash_bytes()` の戻り値をどう扱うか。
  - `zend_ulong` はZend HashTableとの親和性がある。
  - 現在の用途がPHP HashTable keyではなく内部hash値なら、`uint64_t` へ寄せられる可能性がある。
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

## Close Criteria

- `transport_core.h` から `php.h` を外すか、外さない理由が明確に記録されている。
- signature変更する場合はC unitとPHPTが通っている。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
