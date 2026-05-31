# PHP/Zend include boundary: common.h policy

- Status: Open
- Created: 2026-05-31
- Branch: TBD
- Owner: Codex
- Parent: docs/issues/closed/2026-05-31-php-zend-include-boundary.md
- Related-Design: docs/design/php-zend-include-boundary.md

## Background

`src/common.h` は現状のprivate convenience headerであり、PHP/Zend、nghttp2、OpenSSL、socket/system header、gRPC status constants、batch op constantsをまとめている。移行期間のaggregateとしては便利だが、新しいheaderが安易に `common.h` を読むと、層ごとの依存が見えにくくなる。

## Goals

- `common.h` に残すものと、narrow headerへ置くものの方針を明文化する。
- 新規headerが `common.h` に依存すべきか判断できる基準を作る。
- 必要ならpure constants headerの追加を検討する。

## Candidate Policy

最終的に `common.h` に残してよいもの:

- C standard headersのうち、本当にproject-wideに使うもの。
- `config.h` のようなbuild config include。
- `config.h` / `php_grpc.h` のようなextension root。
- 複数層でPHP surface互換性として使うgRPC status / operation constants。
- 移行期間のprivate aggregateとして必要な互換include。

`common.h` に追加しないもの:

- 新しいdomain-specific struct。
- nghttp2 callbackやOpenSSL helperだけに必要なinclude。
- diagnostic-only field/helper。
- transport policy constantsで `transport_core.h` に閉じられるもの。
- PHP/Zend base include。`php.h`、`php_ini.h`、`Zend/zend_smart_str.h` などは、PHP boundary headerまたは`.c`が直接読む。

`common.h` は「PHP/Zend型が必要なheaderの標準入口」にはしない。`zend_string`、`zval`、`PHP_METHOD` などが必要なだけのheaderは、必要なPHP/Zend includeを直接読む方向へ寄せる。必要なら `php_extension_common.h` のようなPHP専用の薄いboundary headerを別に作るが、それはpure/common headerとは分ける。

## Possible Implementation

- `docs/design/php-zend-include-boundary.md` と `docs/design/transport-header-boundaries.md` の方針を揃える。
- 必要なら `src/grpc_constants.h` のようなpure constants headerを作り、C unitやpure headerから読めるようにする。
- `common.h` に「PHP/Zend includeと新しいdomain-specific includeを足さない」コメントを追加する。
- 必要ならPHP/Zend boundary用の薄いheaderを別名で作る。

## Non-Goals

- このissueで全headerを一括移行しない。
- `common.h` を削除しない。
- PHP/Zend includeの移動と、各PHP boundary headerの責務変更を混ぜない。
- function bodyやruntime behaviorを変えない。

## Verification

docs-onlyなら `git diff --check`。

headerを追加またはinclude変更する場合:

- `git diff --check`
- `./tools/test/check-c-static-analysis.sh`
- `./tools/test/check-c-unit.sh`
- `./tools/test/check-phpt.sh`

## Decision Log

- 2026-05-31: `common.h` はすぐに消す対象ではないが、最終的にはPHP/Zend関連includeも外す。PHP/Zend boundaryには直接includeまたは別の薄いPHP専用headerを使う。

## Close Criteria

- `common.h` の方針がdesign docまたはheader commentへ反映されている。
- 必要なpure constants headerを作るか、作らない理由が記録されている。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
