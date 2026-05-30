# お手本化: h2_request_headers header split

- Status: Closed
- Created: 2026-05-31
- Branch: codex/exemplar-transport-header-boundary
- Owner: Codex
- Parent: docs/issues/open/2026-05-31-exemplar-transport-header-boundaries.md

## Background

`transport.h` はaggregate headerとして残すが、最初の低リスクな宣言移動として `h2_request_headers` とrequest header builder helperを `src/h2_request_headers.h` へ切り出す。

このblockは明確なdata object、init/free pair、append helperを持っており、connection ownership、response parser、nghttp2 callback dispatchに触らず移動できる。

## Goals

- `h2_request_headers` の型定義を `transport.h` からnarrow headerへ移す。
- request header builder helperの宣言をnarrow headerへ移す。
- `transport.h` はaggregate includeとして互換性を残す。
- function body、call path、inline/cache policy、field orderは変えない。

## Non-Goals

- request header生成ロジックの変更。
- metadata validationやheader orderの変更。
- `h2_connection`、response parser、ownership modelの整理。
- runtime性能改善。

## Benchmark Policy

この作業は宣言移動のみで、runtime code pathを変えないためbefore/after benchmarkは不要。

次の変更を含める場合は、このissueから外して別issueでbefore/afterを取る。

- request header builderの関数境界変更。
- `static inline` 化やmacro化。
- header allocation/cache policy変更。
- request metadata validationやheader order変更。

## Plan

1. `src/h2_request_headers.h` を追加する。
2. `h2_request_headers` と関連helper宣言を移す。
3. `transport.h` から新headerをincludeし、既存consumer互換を保つ。
4. build/static analysis/C unit/PHPTで宣言移動の破損がないことを確認する。

## Progress

- 2026-05-31: issue作成。
- 2026-05-31: `src/h2_request_headers.h` を追加し、`h2_request_headers` type、request header builder helper、request header trace helperの宣言を移動。
- 2026-05-31: `transport.h` はaggregate headerとして `h2_request_headers.h` をincludeする形にし、既存consumer互換を維持。
- 2026-05-31: `GRPC_LITE_REQUEST_HEADERS_INLINE_CAPACITY` はrequest header builder固有の定数として `h2_request_headers.h` へ移動。

## Verification

- `git diff --check`: PASS
- `./tools/test/check-c-static-analysis.sh`: PASS
- `./tools/test/check-c-unit.sh`: PASS
- `./tools/test/check-phpt.sh`: PASS

## Decision Log

- 2026-05-31: runtime変更なしの宣言移動として扱い、benchmarkは不要と判断。
- 2026-05-31: `h2_request_headers.h` は `zend_string` と `zval` を扱うPHP-aware boundaryであり、現状の設計では `php.h` 依存を許容する。純粋なHTTP/2 header blockとPHP metadata変換層を分ける場合は、関数境界とownershipが変わるため別issueでbefore/afterを取る。

## Close Criteria

- `h2_request_headers` の型とhelper宣言がnarrow headerへ移っている。
- aggregate `transport.h` 経由の既存consumerが壊れていない。
- `git diff --check`、C static analysis、C unit、PHPTが通っている。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
