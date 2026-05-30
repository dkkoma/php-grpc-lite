# お手本化: test discoverability と gate整理

- Status: Open
- Created: 2026-05-31
- Branch: main
- Owner: Codex
- Parent: docs/issues/open/2026-05-31-c-php-extension-exemplar-structure.md

## Background

PHPT、PHPUnit、C unit、fuzz、sanitizer、ZTS、release hardeningの層は強いが、学習者にはPHPT/PHPUnitの使い分け、長いPHPTのscenario境界、CIで何が必須gateなのかが見えづらい。

特に `tests/phpt/020-request-metadata-control.phpt` と `tests/phpt/024-control-semantics.phpt` は回帰テストとして有用だが、複数scenarioが1ファイルに詰まっている。

## Goals

- 長いPHPTのscenario境界を読みやすくする。
- PHPT / PHPUnit / release hardening / measurement-only scriptの役割を明確にする。
- PHPUnit integration suiteをCIまたはmanual/scheduled gateへどう置くか決める。
- lifecycle / slow-consumer scriptsをgateにするかmeasurement-onlyにするか決める。

## Non-Goals

- C transport実装の変更。
- fixture server behaviorの意味変更。
- performance改善。
- Spanner emulatorの性能ベンチ化。

## Plan

1. `020-request-metadata-control.phpt` をpositive metadata round-trip、invalid metadata、mutation/isolationなどに分けるか、ファイル内scenario tableを追加するか決める。
2. `024-control-semantics.phpt` をconnection refusal、GOAWAY/EOF、RST、abandoned streamなどに分けるか、ファイル内scenario tableを追加するか決める。
3. `phpunit.xml.dist` と `.github/workflows/native-qa.yml` を確認し、fast non-Spanner PHPUnitとSpanner emulator PHPUnitの配置案を作る。
4. `check-native-slow-consumer.sh` と lifecycle stress scriptsを確認し、threshold付きgateかmeasurement-onlyかを決める。
5. 必要ならdocsとscript名を更新する。

## Performance Notes

- PHPT分割自体は性能影響なし。
- PHPUnit CI追加はCI実行時間、service readiness、Spanner emulator flake率に影響するため、導入前に想定実行時間と対象groupを記録する。
- lifecycle / slow-consumerをthreshold付きgateにする場合、環境依存flakeを避けるため閾値と実行環境を明記する。

## Progress

- 2026-05-31: 親issueからtest discoverability作業を子issue化。

## Verification

変更内容に応じて:

- `./tools/test/check-phpt.sh`
- `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit`
- `git diff --check`
- CI追加時はworkflow durationと失敗時の診断ログ確認

## Decision Log

- 2026-05-31: coverage量の追加より、まずscenario境界とgate責務の明確化を優先する。

## Close Criteria

- 長いPHPTのscenario境界が分かる形になっている。
- PHPUnit integration suiteのCI/manual/scheduled上の扱いが決まっている。
- lifecycle / slow-consumerがgateまたはmeasurement-onlyとして明記されている。
- 必要な検証結果を追記し、`Status: Closed` にして `docs/issues/closed/` へ移動する。
