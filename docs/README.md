# Documentation Guide

このディレクトリの入口です。`php-grpc-lite` の現在の設計、実装の読み方、検証、ベンチ結果、作業履歴はここから辿ります。

まず現在の設計を知りたい場合は [SPEC.md](./SPEC.md)、実装を読みたい場合は [code-reading-guide.md](./guides/code-reading-guide.md)、検証観点を確認したい場合は [verification-matrix.md](./verification/verification-matrix.md) から読む。

## Current Design

| Document | Role |
|---|---|
| [SPEC.md](./SPEC.md) | 現在の仕様、スコープ、runtime transport方針、API互換性の基準 |
| [api-surface.md](./design/api-surface.md) | `google/gax` / `google/cloud-*` が使う `Grpc\` surfaceの互換性一覧 |
| [http2-transport-decision.md](./design/http2-transport-decision.md) | libcurl / PoC / nghttp2比較後にHTTP/2 transport 1系統へ決めた判断 |
| [http2-transport-design.md](./design/http2-transport-design.md) | nghttp2 + socket/TLS transportの現在の設計 |
| [transport-header-boundaries.md](./design/transport-header-boundaries.md) | `transport.h`、`common.h`、transport internal headerの境界方針 |
| [grpc-call-exchange-state.md](./design/grpc-call-exchange-state.md) | `grpc_call` field、connection / stream ownership、hot path上の責務map |
| [protocol-classification-boundary.md](./design/protocol-classification-boundary.md) | gRPC protocol failure分類とHTTP/2 transport actionの責務境界 |

## Reading Guides

| Document | Role |
|---|---|
| [code-reading-guide.md](./guides/code-reading-guide.md) | 初学者 / 中級者 / 上級者向けの実装読み順 |
| [install-native-extension.md](./guides/install-native-extension.md) | source build / PIE install / rollback / large streaming guidance |
| [opentelemetry-instrumentation.md](./guides/opentelemetry-instrumentation.md) | benchmark用OTEL spanとtrace context metadata注入の方針 |

## Verification

| Document | Role |
|---|---|
| [native-test-framework.md](./verification/native-test-framework.md) | C unit、PHPT、fuzz、PHPUnit integrationの役割分担 |
| [test-fixtures.md](./verification/test-fixtures.md) | Go test-server / raw lifecycle fixtureのport、trigger metadata、期待挙動 |
| [verification-matrix.md](./verification/verification-matrix.md) | HTTP/2 / gRPC semanticsと検証層の対応 |
| [compatibility-control-checklist.md](./verification/compatibility-control-checklist.md) | metadata/status/deadline/control semanticsの互換性チェック |
| [protocol-model-review-guide.md](./verification/protocol-model-review-guide.md) | HTTP/2/gRPC transport変更時のdomain model review観点 |
| [release-qa-checklist.md](./verification/release-qa-checklist.md) | release readinessと残gate |

## Records

| Directory | Role |
|---|---|
| [benchmarks/](./benchmarks/) | 現行benchmarkの実行方針と計測結果。入口は [benchmarks/README.md](./benchmarks/README.md) |
| [issues/](./issues/) | 作業issue、設計判断、進捗。入口は [issues/README.md](./issues/README.md) |
| [reviews/](./reviews/) | サブエージェントレビュー指摘と対応履歴。入口は [reviews/README.md](./reviews/README.md) |
| [research/](./research/) | 過去の調査、PoC、比較記録。current design docではなく履歴資料として扱う |

## Placement Policy

新しいdocを追加する前に、まず既存docへ統合できるか確認する。

- current designを変える内容は、原則として [SPEC.md](./SPEC.md) または該当するdesign docへ反映する。
- 実装を読むための道案内は [code-reading-guide.md](./guides/code-reading-guide.md) に置く。
- 検証対象、fixture、release gateは verification系docへ置く。
- benchmarkの実行方針と結果は [benchmarks/](./benchmarks/) へ置く。
- 作業途中の判断、採否、検証結果は [issues/](./issues/) へ置く。
- review指摘と修正履歴は [reviews/](./reviews/) へ置く。
- 過去のPoCや調査結果は [research/](./research/) へ置き、current design docへ混ぜない。

独立docを作るのは、次の条件を満たす場合に限る。

- 既存docへ入れると、責務が混ざって読み順が崩れる。
- 入口から明確にリンクできる。
- ownerとなる分類が分かる。
- 近い内容のdocとの差分が説明できる。

## Directory Policy

`docs/` 直下には、入口の [README.md](./README.md) と最上位仕様の [SPEC.md](./SPEC.md) を置く。

- `design/`: current design、設計判断、実装内部の責務map
- `guides/`: 読み方、install、運用手順、instrumentation guide
- `verification/`: test fixture、verification matrix、review gate、release QA
- `benchmarks/`: benchmark runner方針と計測結果
- `issues/`: 作業issue、設計判断、進捗
- `reviews/`: review指摘と対応履歴
- `research/`: 過去のPoCや調査結果

既存docをさらに移動する場合は、独立issueでpath移動、リンク更新、旧path参照確認をまとめて行う。
