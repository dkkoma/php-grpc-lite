# お手本化: transport header boundary整理

- Status: Open
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
- `static inline` 化、関数境界変更、構造体field配置変更、request/response hot pathの呼び出し変更を入れる場合は、このissueではなく性能計測付きの別issueに切り出す。

## Progress

- 2026-05-31: 親issueからtransport header boundary作業を子issue化。

## Verification

- `./tools/test/check-c-static-analysis.sh`
- `./tools/test/check-c-unit.sh`
- `./tools/test/check-phpt.sh`
- 通常build / bench build load
- `git diff --check`
- HTTP/2/gRPC domain model review

## Decision Log

- 2026-05-31: このissueでは挙動変更なしのheader boundary整理を主対象にする。

## Close Criteria

- `src/transport.h` の責務が分割または分割方針として明確になっている。
- `src/common.h` のinclude方針が改善されている、または後続issue化されている。
- production / diagnostic boundaryが崩れていない。
- domain model reviewでBlocker / High / Medium / Lowがnoneになる。
- 検証結果を追記し、`Status: Closed` にして `docs/issues/closed/` へ移動する。
