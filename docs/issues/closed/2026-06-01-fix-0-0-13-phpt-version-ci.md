# 0.0.13 version PHPT expectation CI fix

Status: Closed
Branch: codex/fix-0-0-13-phpt-version
Target-Release: 0.0.13 follow-up
Fix-Commit: このissueを閉じる修正commit

## 目的

`f6b6612` 以降の Native QA 失敗を復旧する。

## 背景

`f6b6612` で `PHP_GRPC_VERSION` を `0.0.13` に更新したが、`tests/phpt/001-load.phpt` の `phpversion("grpc")` と `Grpc\VERSION` の期待値が `0.0.12` のままだった。

GitHub Actions の最初の失敗 run は `26727824623`。`Development gate`、`C coverage`、`ZTS PHPT` はいずれも `tests/phpt/001-load.phpt` で失敗している。

## スコープ

- `tests/phpt/001-load.phpt` の runtime version 期待値を `0.0.13` に更新する。
- 修正後に PHPT を再実行する。

## 非スコープ

- runtime version policy の変更。
- release tag や release asset の作り直し。
- production code の変更。

## 計画

1. CI ログで失敗箇所を確認する。
2. PHPT の期待値を `PHP_GRPC_VERSION` と同じ `0.0.13` に揃える。
3. Docker compose 内で PHPT を実行する。
4. issue を閉じ、修正commitを作成する。

## 進捗

- 2026-06-01: `f6b6612` の GitHub Actions run `26727824623` を確認。全失敗jobが `tests/phpt/001-load.phpt` に集約されることを確認。
- 2026-06-01: `tests/phpt/001-load.phpt` の version 期待値を `0.0.13` に更新。
- 2026-06-01: NTS PHPT、ZTS PHPT、C coverage を通し、CI失敗箇所が復旧していることを確認。

## 検証

- `./tools/test/check-phpt.sh`: PASS
  - 15 tests, 15 passed
- `./tools/test/check-zts-phpt.sh`: PASS
  - 15 tests, 15 passed
- `./tools/test/check-c-coverage.sh`: PASS
  - C unit: PASS
  - PHPT: 15 tests, 15 passed
  - line coverage: 76.5% (2637 / 3445)
  - function coverage: 95.0% (210 / 221)

## 判断ログ

- production code はすでに `0.0.13` として動いているため、修正対象は test expectation のみ。
- version bump 時にこの PHPT 期待値を更新し忘れたことが原因で、CI failure は `0.0.13` runtime自体の挙動不良ではない。
- `tests/`、`README.md`、`docs/guides/install-native-extension.md`、`php_grpc.h` に残る `0.0.12` の現在version期待値はないことを `rg` で確認した。過去issueの履歴に残る `0.0.12` は修正対象外。

## 完了条件

- `tests/phpt/001-load.phpt` が `0.0.13` を期待する。
- `./tools/test/check-phpt.sh` が通る。
- CI復旧内容と検証結果を記録して issue を `closed/` に移動する。
