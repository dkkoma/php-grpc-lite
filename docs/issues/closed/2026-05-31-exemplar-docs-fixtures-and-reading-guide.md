# お手本化: fixture / verification / reading guide整備

- Status: Closed
- Created: 2026-05-31
- Branch: main
- Owner: Codex
- Parent: docs/issues/open/2026-05-31-c-php-extension-exemplar-structure.md

## Background

`php-grpc-lite` はC/PHP拡張として実戦的な検証層を持っているが、test-server fixture、`x-bench-*` metadata control protocol、raw fixture ports、PHPT / PHPUnit / C unit / fuzz / sanitizer / ZTS gateの対応関係が分散している。

実装を変えずに、初学者から上級者までが「どこを読めば何が分かるか」を追えるようにする。

## Goals

- `docs/verification/test-fixtures.md` を追加し、fixture port、service mode、trigger metadata、expected behavior、owning testsを一覧化する。
- `docs/verification/verification-matrix.md` を追加し、主要HTTP/2/gRPC semanticsと検証層の対応を一覧化する。
- `docs/guides/code-reading-guide.md` に beginner / intermediate / advanced の読み順を追加する。
- `tests/fuzz/README.md` を追加し、fuzz harness selector、seed corpus、CI smokeの意味を説明する。

## Non-Goals

- C実装やfixture serverの挙動を変更しない。
- PHPT / PHPUnit の分割やCI追加はこのissueでは扱わない。
- coverageを増やすこと自体を目的にしない。まず可視化する。

## Plan

1. `poc/test-server/main.go`、`tests/phpt/*`、`tests/Integration/*`、`tools/test/*` を参照してfixture catalogを作る。
2. `docs/verification/protocol-model-review-guide.md` と `docs/verification/compatibility-control-checklist.md` の観点をverification matrixへ落とす。
3. `docs/guides/code-reading-guide.md` に習熟度別の読み順を追加する。
4. `tests/fuzz/README.md` にselector byteとseed corpusの役割を記録する。

## Progress

- 2026-05-31: 親issueからdocumentation-first作業を子issue化。
- 2026-05-31: `docs/verification/test-fixtures.md` を追加し、test-server ports、service methods、metadata controlsを一覧化。
- 2026-05-31: `docs/verification/verification-matrix.md` を追加し、主要semanticsと検証層の対応を一覧化。
- 2026-05-31: `docs/guides/code-reading-guide.md` に初学者 / 中級者 / 上級者向けの読み順を追加。
- 2026-05-31: `tests/fuzz/README.md` を追加し、fuzz selectorとseed corpusを説明。

## Verification

- `git diff --check`: PASS
- ドキュメント変更のみ。C実装・fixture server・test behaviorは未変更のため、PHPT/PHPUnitは未実行。

## Close Summary

実装変更なしで、fixture / verification / reading guide / fuzz corpusの発見性を改善した。

## Decision Log

- 2026-05-31: このissueは性能影響なしのドキュメント整備として扱う。
- 2026-05-31: matrix上で薄いcoverageはこのissueでは埋めず、該当子issueまたは将来issueのfollow-upとして明示する。

## Close Criteria

- fixture port `50051`-`50060` と主要 `x-bench-*` triggerがドキュメント化されている。
- unary / server streamingの主要semanticsと検証層の対応がmatrix化されている。
- code reading guideに習熟度別の読み順がある。
- fuzz corpus semanticsがREADME化されている。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
