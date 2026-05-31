# PHP/Zend include boundary見直し

- Status: Closed
- Created: 2026-05-31
- Branch: codex/php-zend-include-boundary
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

このissueでは、本来あるべき姿と現状差分を `docs/design/php-zend-include-boundary.md` にまとめ、実作業は次の子issueへ分ける。

| Child issue | Scope | Performance risk |
|---|---|---|
| `docs/issues/open/2026-05-31-php-zend-include-boundary-mechanical-narrowing.md` | `tls_config.h`、`grpc_result.h`、`wrapper_adapter.h`、`diagnostic/bench_call.h` などの機械的include narrowing | なし。runtime挙動、signature、layoutを変えない |
| `docs/issues/open/2026-05-31-php-zend-include-boundary-transport-core-scalar.md` | `transport_core.h` の `zend_long` / `zend_ulong` 依存をC scalar boundaryへ寄せる | 低。signature変更あり、C unit/PHPT必須 |
| `docs/issues/open/2026-05-31-php-zend-include-boundary-common-policy.md` | `common.h` に残すもの、narrow headerへ逃がすもの、pure constants header要否を決める | なしから低。include変更するならbuild gate必須 |
| `docs/issues/open/2026-05-31-php-zend-include-boundary-metadata-exchange-split.md` | request/response metadata conversionとexchange stateのdomain boundary split検討 | 高。before/after benchmarkとdomain model review必須 |

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
- 2026-05-31: 専用ブランチ `codex/php-zend-include-boundary` を作成。
- 2026-05-31: `src/common.h`、`transport_core.h`、`tls_config.h`、`grpc_exchange_state.h`、`h2_request_headers.h`、`transport.h`、PHP surface/diagnostic headersの依存をコードレベルで確認。
- 2026-05-31: 本来あるべき層構造、現状差分、approach orderを `docs/design/php-zend-include-boundary.md` に追加。
- 2026-05-31: 実作業を4本の子issueへ分割。

## Completion Summary

このissueでは、`php.h` 依存削減を「とにかくPHP/Zend includeを消す」作業として扱わず、C/PHP extensionとして自然な境界へ整理した。

判断の要点:

- PHP/Zend object lifecycle、`zval`、`zend_string` ownershipを持つ層ではPHP/Zend依存を許容する。
- ただし、PHP/Zend依存を許容することと `common.h` を読むことは同義ではない。最終的には `common.h` からPHP/Zend関連includeを外し、PHP boundary headerが必要なPHP/Zend includeを直接読む。
- `protocol_core.h` のようなpure C層は現状維持し、PHP/Zend依存を持ち込まない。
- `transport_core.h` はtransport policy helperとしてPHP/Zend依存を外す価値が高い。これは単なるinclude整理ではなく、PHP scalar boundaryの設計変更として扱う。
- `tls_config.h` のようにOpenSSLだけが必要なheaderは、mechanical include narrowingの最初の候補にする。
- `h2_request_headers` や `grpc_exchange_state` の分割は、見通し改善の余地はあるがhot pathやownershipへ触るため、performance-sensitive issueとして扱う。

## Verification

- docs-only。
- `git diff --check` を実行する。

## Decision Log

- 2026-05-31: `php.h` 依存削減は `h2_request_headers` 単体ではなく、header boundary全体の見直しとして扱う。
- 2026-05-31: PHP/Zend型を扱うboundaryでは `php.h` 依存を許容する。一方でpure C / protocol coreへ不要なPHP/Zend依存を広げない。
- 2026-05-31: 最終ゴールでは `common.h` からPHP/Zend関連includeを外す。PHP/Zend依存が必要なheaderは、`common.h` ではなく直接includeまたはPHP専用boundary headerを使う。
- 2026-05-31: 機械的include narrowing、PHP scalar boundary整理、`common.h` policy、metadata/exchange state splitを混ぜずに子issueへ分ける。特にmetadata/exchange state splitはbefore/afterなしに進めない。

## Close Criteria

- headerごとのPHP/Zend依存の分類が完了している。
- `common.h` include policyの改善方針がdocsまたは子issueへ反映されている。
- 挙動変更なしでできるinclude整理と、performance-sensitiveなboundary分割が切り分けられている。
- 必要な子issueが作成されている。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
