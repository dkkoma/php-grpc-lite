# お手本化: transport header boundary整理

- Status: Closed
- Created: 2026-05-31
- Branch: main
- Owner: Codex
- Parent: docs/issues/open/2026-05-31-c-php-extension-exemplar-structure.md

## Background

`src/transport.h` は現在、`h2_connection`、persistent cache、server streaming resource state、nghttp2 callbacks、socket/TLS helpers、request header builder、response metadata/status helpers、cleanup helpersまで広く公開している。

これは旧 `internal.h` 一極集中より大きく改善しているが、Cプロジェクトのお手本としてはまだ「transport.h が新しい巨大internal header」になりやすい。

## Goals

- `src/transport.h` を責務別ヘッダへ段階的に分割する。
- `src/common.h` の巨大includeを薄くする方針を作る。
- public C APIを追加せず、internal APIの見通しだけを良くする。
- まずは挙動変更なしの宣言移動・include整理として進める。

## Non-Goals

- HTTP/2 transportの挙動変更。
- `grpc_call` field分割。
- ownership modelの抽象化。
- performance改善。
- public install headerの提供。

## Plan

1. `src/transport.h` の宣言を分類する。
   - connection / cache
   - callbacks
   - request headers
   - response parser / metadata
   - status result bridge
   - socket/TLS I/O
   - server streaming resource state
2. 分割候補ヘッダを決める。
   - `h2_connection.h`
   - `persistent_connection_cache.h`
   - `h2_request_headers.h`
   - `h2_callbacks.h`
   - `grpc_response_parser.h`
   - `grpc_metadata.h`
   - `transport_io.h`
3. まずは宣言移動とinclude整理だけを行い、関数本体やinline/cache policyを変えない。
4. `src/common.h` のincludeを実利用に寄せて薄くできる箇所を整理する。
5. C static analysis、C unit、PHPT、bench build loadを確認する。

## Performance Notes

- 宣言移動とinclude整理だけならruntime性能影響は想定しない。
- `static inline` 化、function boundary変更、構造体field配置変更、request/response hot pathの呼び出し変更を入れる場合は、このissueではなく性能計測付きの別issueに切り出す。

### Benchmark Policy

このissueで許容する実装は「宣言移動」「include整理」「aggregate header維持」に限定する。この範囲ではruntime benchmarkを採否条件にしない。代わりにbuild、static analysis、C unit、PHPTでinclude boundaryの破損がないことを確認する。

少しでも次に触れる場合は、このissue内で続けず、before/after付きの子issueへ切り出す。

| Change | Existing benchmark | Add case if needed |
|---|---|---|
| request header builderの関数境界変更 | `./bench/run.sh metadata-header`, `./bench/run.sh spanner-shape` | `cpu-micro` に metadata unary caseを追加 |
| response parser / metadata callbackの関数境界変更 | `./bench/run.sh metadata-header`, `./bench/run.sh payload-streaming`, `./bench/run.sh large-streaming` | DATA fragmentation diagnosticを追加 |
| socket/TLS I/O helperの関数境界変更 | `./bench/run.sh throughput-unary`, `./bench/run.sh rtt-unary`, `./bench/run.sh tls-spanner-shape` | TLS write/read attribution用の小caseを追加 |
| struct field配置、inline化、cache policy変更 | このissueでは扱わない | 専用issueでlayout dumpとbefore/afterを定義 |

## Progress

- 2026-05-31: 親issueからtransport header boundary作業を子issue化。
- 2026-05-31: `docs/design/transport-header-boundaries.md` を追加し、現在のconsumer、target header group、`common.h` include policy、safe first splitを整理。
- 2026-05-31: 実装の宣言移動は未実施。`h2_request_headers` などから小さく分割する方針を記録し、issueはopenのまま継続する。
- 2026-05-31: 最初の実装stepとして `docs/issues/closed/2026-05-31-exemplar-h2-request-headers-header-split.md` を完了。`h2_request_headers` typeとrequest header helper宣言をnarrow headerへ移動。
- 2026-05-31: `persistent_connection_cache` header splitは検討したが、`h2_connection` lifetime、module globals、surface側key生成、transport側preflight/detachがまたがり、headerだけ分けると実装ownerが曖昧になるためreject。
- 2026-05-31: これ以上の細かいheader分割は、このissueでは進めない。残る読みづらさは `connection / stream ownership model` issueでstate machineとownership invariantとして扱う。

## Verification

- `git diff --check`: PASS
- `h2_request_headers` 宣言分離: `./tools/test/check-c-static-analysis.sh`, `./tools/test/check-c-unit.sh`, `./tools/test/check-phpt.sh` PASS。詳細は `docs/issues/closed/2026-05-31-exemplar-h2-request-headers-header-split.md`。
- このclose commitはdocs-only。C実装、header include、struct layout、callback pathは変更しないため追加benchmarkは不要。
- HTTP/2/gRPC domain model review: docs-only boundary reviewとして、production / diagnostic boundary、connection / stream / call / resource scopeを `docs/verification/protocol-model-review-guide.md` の観点で確認。実装変更なしのためBlocker / High / Medium / Lowはnone。

## Decision Log

- 2026-05-31: このissueでは挙動変更なしのheader boundary整理を主対象にする。
- 2026-05-31: `transport.h` をすぐ分割しない。まずaggregate headerを残したままnarrow headerを追加する方針とし、declaration moveごとに小issue/小コミットへ分ける。
- 2026-05-31: runtime性能影響を避けるため、field order変更、`static inline` 化、callback/parser実装変更はこのissueの対象外とする。
- 2026-05-31: headerは細かく分ければお手本になるわけではない。明確なdata objectとinit/free pairを持つ `h2_request_headers` は分離を採用したが、persistent connection cacheはcross-cuttingなownershipを隠すだけになるため採用しない。

## Close Criteria

- `src/transport.h` の責務が分割または分割方針として明確になっている。
- `src/common.h` のinclude方針が改善されている、または後続issue化されている。
- production / diagnostic boundaryが崩れていない。
- domain model reviewでBlocker / High / Medium / Lowがnoneになる。
- 検証結果を追記し、`Status: Closed` にして `docs/issues/closed/` へ移動する。
