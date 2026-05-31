# PHP/Zend include boundary見直し

- Status: Open
- Created: 2026-05-31
- Branch: codex/exemplar-php-zend-include-boundary
- Owner: Codex

## Background

`src/h2_request_headers.h` の分離で、request header builderが `zend_string` と `zval` を扱うため `php.h` に依存することが明確になった。これはそのheaderだけの問題ではなく、`src/common.h` がPHP/Zend、nghttp2、OpenSSL、socket/system headerを広くincludeしているため、複数のinternal headerが必要以上にPHP extension runtimeへ結合しやすい構造になっている。

C/PHP拡張のお手本としては、次の境界を読める状態にしたい。

- pure C / protocol core
- transport core policy
- HTTP/2 transport object
- PHP/Zend object lifecycle
- PHP metadata / zval conversion
- diagnostic / bench-only boundary

## Current Observations

- `src/common.h` は `php.h`、Zend headers、`nghttp2/nghttp2.h`、OpenSSL、socket/system headersをまとめてincludeしている。
- `src/protocol_core.h` と `src/status_core.h` は比較的pure C寄りで、PHP/Zend型を直接持たない。
- `src/transport_core.h` は `zend_long` / `zend_ulong` を使うため `php.h` に依存しているが、実体はtransport policy / validation helperに近い。
- `src/grpc_exchange_state.h` は `zend_string`、`smart_str`、`struct iovec`、`grpc_call` exchange stateを持つためPHP/Zendとsystem I/Oの両方に依存する。
- `src/h2_request_headers.h` は `nghttp2_nv`、`zend_string`、`zval` を持つため、純粋なHTTP/2 header blockではなくPHP metadata to HTTP/2 header conversion boundaryである。
- `src/grpc_result.h`、`src/surface.h`、`src/module.h`、`src/wrapper_adapter.h` はPHP extension surfaceなのでPHP/Zend依存が自然。
- `src/tls_config.h` はOpenSSL中心だが、現状は `common.h` 経由でPHP/Zendにも依存する。

## Goals

- internal headerを「PHP/Zend依存が自然な層」と「できればpure C寄りに保ちたい層」に分類する。
- `common.h` の役割をprivate convenience headerから、より薄い基盤headerへ寄せる方針を決める。
- `php.h` 依存を減らす候補を、挙動変更なしの宣言/include整理と、設計変更を伴うboundary分割に分ける。
- 初学者にも「PHP拡張境界ではPHP/Zend型が自然だが、protocol coreへ持ち込まない」ことが分かる構造にする。

## Non-Goals

- このissueだけでinclude整理を一括実装しない。
- `grpc_call` field layoutを変えない。
- request metadata変換、response metadata変換、status semanticsを変えない。
- `zend_string` ownershipやzval lifetimeを変えない。
- performance改善を目的にしない。

## Proposed Classification

| Header | Current dependency | Desired classification | Notes |
|---|---|---|---|
| `protocol_core.h` | C stdint/stdbool | Pure C | 維持する |
| `status_core.h` | `struct _grpc_call` forward only | Mostly pure C over exchange state | `grpc_call`依存をDTO化できるかは別途検討 |
| `transport_core.h` | `php.h` for `zend_long` / `zend_ulong` | Transport policy, PHP scalar boundaryあり | `zend_long`を内部aliasへ寄せる余地あり |
| `h2_request_headers.h` | `php.h`, nghttp2 | PHP metadata to HTTP/2 header boundary | `zval`を受ける限りPHP-awareでよい |
| `grpc_exchange_state.h` | `common.h` | Exchange state with PHP-owned buffers | hot pathかつlayout sensitive。分離は別issue |
| `grpc_result.h` | `common.h` | PHP result bridge | PHP/Zend依存が自然 |
| `transport.h` | aggregate | Transport aggregate | narrow header移行中だけ広くてよい |
| `tls_config.h` | `common.h` | OpenSSL helper | PHP/Zend不要にできる可能性あり |
| `surface.h` / `module.h` / `wrapper_adapter.h` | PHP/Zend | PHP extension surface | PHP/Zend依存が自然 |
| `diagnostic/*.h` | bench/PHP types | Diagnostic boundary | production coreへ逆流させない |

## Candidate Follow-up Tasks

1. `tls_config.h` のincludeを薄くする
   - `common.h` ではなくOpenSSLとstddef中心にできるか確認する。
   - runtime挙動変更なしならbenchmark不要。

2. `transport_core.h` のPHP scalar dependencyを整理する
   - `zend_long` / `zend_ulong` を使う理由を明文化する。
   - PHP INI / surface scalarをcore helperへ渡す境界として許容するか、内部型へ変換してから渡すかを判断する。
   - signature変更を伴う場合はC unit中心に検証する。

3. `h2_request_headers` を純HTTP/2 header blockとPHP metadata conversionへ分けるか検討する
   - これは関数境界、ownership、allocation policyが変わる可能性がある。
   - 進める場合はrequest header builder performance issueとしてbefore/afterを取る。

4. `grpc_exchange_state.h` のPHP/Zend buffer依存をfield mapに沿って整理する
   - `zend_string` / `smart_str` / queue payload ownershipを含むため、`grpc_call` field layout issueと連動する。
   - layout変更を伴う場合はbefore/after必須。

5. `common.h` のinclude policyをdocsへ反映する
   - 何を入れてよいか、何をnarrow headerへ置くかを明文化する。
   - docs-onlyならbenchmark不要。

## Benchmark Policy

include整理だけならruntime benchmarkは不要。build/static analysis/C unit/PHPTを採否条件にする。

次の変更を含める場合は別issueでbefore/afterを取る。

- request header builderの関数境界、allocation policy、metadata validation変更。
- `grpc_call` field layout、sub-struct化、buffer ownership変更。
- response metadata/status変換の遅延化やDTO化。
- transport hot pathの関数呼び出し追加やinline化。

既存benchmarkで最初に見る候補:

- request header boundary: `./bench/run.sh metadata-header`, `./bench/run.sh spanner-shape`
- exchange state / parser: `./bench/run.sh cpu-micro`, `./bench/run.sh payload-streaming`, `./bench/run.sh large-streaming`
- TLS/OpenSSL boundary: `./bench/run.sh tls-spanner-shape`, `./bench/run.sh rtt-unary`

足りない場合は、該当issueでsmall caseを追加してから実装する。

## Plan

1. headerごとの依存とDesired classificationをレビューする。
2. `common.h` に残すもの、narrow headerへ移すもの、別issueへ切るものを決める。
3. 挙動変更なしでできるinclude整理から小さいissueへ分割する。
4. 関数境界やownershipに触る候補はperformance-sensitive issueとしてbefore/afterを定義する。

## Progress

- 2026-05-31: `h2_request_headers.h` の `php.h` 依存をきっかけにissue化。

## Verification

- issue作成のみ。コード変更なし。

## Decision Log

- 2026-05-31: `php.h` 依存削減は `h2_request_headers` 単体ではなく、header boundary全体の見直しとして扱う。
- 2026-05-31: PHP/Zend型を扱うboundaryでは `php.h` 依存を許容する。一方でpure C / protocol coreへ不要なPHP/Zend依存を広げない。

## Close Criteria

- headerごとのPHP/Zend依存の分類が完了している。
- `common.h` include policyの改善方針がdocsまたは子issueへ反映されている。
- 挙動変更なしでできるinclude整理と、performance-sensitiveなboundary分割が切り分けられている。
- 必要な子issueが作成されている。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
