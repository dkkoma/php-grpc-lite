# Native QA job boundaries

Status: Open
Branch: codex/native-qa-job-boundaries

## 目的

GitHub Actions の `Native QA` を、`Development gate` という詰め合わせjobから目的別jobへ整理する。

## 背景

現在の `Development gate` は static analysis、C unit、NTS PHPT、短時間の生成入力checkをまとめて実行している。一方で `ZTS PHPT` は別jobで、`C coverage` もNTS PHPTとC unitを別buildで再実行している。

この構造では、CI画面上で何を保証しているのかが読みづらく、NTS PHPTとC unitも重複して実行される。

また `C fuzz smoke` という名前は手段の名前であり、CI利用者には何を失敗として検出するのかが見えにくい。目的は crash / undefined behavior の検出であるため、CI上の名前は `Crash/UB check` にする。

## スコープ

- `Native QA` workflowを目的別jobへ分ける。
  - `Static analysis`
  - `NTS PHPT + C coverage`
  - `ZTS PHPT`
  - `Crash/UB check`
- NTS PHPTとC unitは `NTS PHPT + C coverage` に寄せ、`Development gate` jobでの二重実行をやめる。
- `check-c-fuzz.sh` を `check-crash-ub.sh` に改名する。
- local向けの詰め合わせrunner `check-native-development-gate.sh` を削除し、目的別runnerを直接使う形にする。
- 現行入口から参照されない古い手動diagnostic scriptを削除する。
- 現行docsの検証入口と説明を更新する。

## 非スコープ

- `tests/fuzz/` harnessやseed corpusの構造変更。
- coverageの採否基準変更。
- PHPUnit integrationのNative QA必須化。
- release hardening gateの内容追加。

## 計画

1. `Native QA` workflowを目的別jobへ分ける。
2. Crash/UB runner名と呼び出し元を更新する。
3. READMEとverification docsを更新する。
4. workflow YAMLとrunnerを検証する。
5. branch CIで `Native QA` を確認する。

## 進捗

- 2026-06-02: issue作成。
- 2026-06-02: `Native QA` を `Static analysis`、`NTS PHPT + C coverage`、`Crash/UB check`、`ZTS PHPT` に分解。
- 2026-06-02: `check-c-fuzz.sh` を `check-crash-ub.sh` に改名し、local/release runnerの呼び出しを更新。
- 2026-06-02: `check-native-development-gate.sh` を削除し、local docsを目的別runnerの案内へ更新。
- 2026-06-02: 未参照の `tools/diagnostics/laravel-fpm-spanner-bench/run-vm-compare.sh` を削除。

## 検証

- `ruby -e 'require "yaml"; YAML.load_file(".github/workflows/native-qa.yml"); puts "native-qa yaml ok"'`: PASS
- `bash -n tools/test/check-c-static-analysis.sh tools/test/check-crash-ub.sh tools/test/check-native-release-hardening.sh`: PASS
- `docker compose config --quiet`: PASS
- `./tools/test/check-c-static-analysis.sh`: PASS
- `FUZZ_RUNS=100 ./tools/test/check-crash-ub.sh`: PASS
- `rg -n "check-native-development-gate|run-vm-compare|check-c-fuzz|C fuzz smoke|fuzz smoke|libFuzzer smoke" README.md docs/README.md docs/guides docs/verification tests/fuzz tools .github -g '*.md' -g '*.sh' -g '*.yml'`: no matches
- tracked shell scriptの未参照確認: 追加の未参照scriptなし

## 判断ログ

- `Development gate` というjob名は、ZTSが外に出ている状態では責務が曖昧なため廃止する。
- NTS PHPTはcoverage instrumentation付きbuildでの実行をCI上のNTS代表にする。通常buildのNTS PHPTを別jobで残すと、主に重複が増えるため採用しない。
- `Crash/UB check` は手段ではなく失敗条件を表す名前として採用する。現時点の対象は `protocol_core` だが、将来対象を増やしても名前が破綻しない。

## 完了条件

- `Native QA` が目的別jobに分かれている。
- `Development gate` jobがなくなっている。
- `check-crash-ub.sh` がCIとlocal runnerから使われている。
- `check-native-development-gate.sh` が削除され、現行docsから参照されていない。
- 削除したdiagnostic scriptへの現行参照が残っていない。
- branchで `Native QA` が通る。
