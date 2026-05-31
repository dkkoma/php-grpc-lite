# Dead code audit

- Status: Open
- Created: 2026-05-31
- Branch: TBD
- Owner: Codex
- Parent: none
- Related-Design: docs/design/php-zend-include-boundary.md

## Background

`hash_bytes()` は過去にpersistent connection identity照合で使われていたが、SHA-256 connection keyへ移行した後も `transport_core` とC unitに残っていた。今回のPHP/Zend include境界整理で見つかったため、同種の未使用helper、古いdiagnostic hook、過去の最適化実験の残骸が他にもないかを独立して確認したい。

この調査はPHP/Zend include境界issueとは独立させる。目的はコードを減らすこと自体ではなく、production path、diagnostic/bench path、test-only pathの境界を明確にし、初学者にも上級者にも「なぜ残っているか」を説明できる状態にすることである。

## Goals

- C実装、header、diagnostic/bench、testsに残る未使用または用途不明のsymbolを洗い出す。
- 削除できるもの、test-onlyへ閉じ込めるもの、production/benchのため残すものを分類する。
- 削除候補ごとに、削除前後の検証方法を明確にする。
- 静的解析で検出できる範囲と、手動確認が必要な範囲を記録する。

## Investigation Targets

- `src/*.c` / `src/*.h` の関数、struct field、macro、constant。
- `src/diagnostic/*` のbench-only helperとproduction coreからの逆依存。
- `tests/unit/*` だけが参照しているhelper。
- PHPTやPHPUnitからだけ到達するPHP-visible function、method、INI、class entry。
- `#ifdef PHP_GRPC_LITE_ENABLE_BENCH` やbuild optionで片側だけ使われるsymbol。
- 過去の最適化やdiagnosticで導入され、現在のproduction pathから外れたhelper。

## Static Analysis Notes

静的解析で見つけやすいもの:

- `static` function / variableの未使用。
- `-Wunused-function`、`-Wunused-variable`、`-Wunused-const-variable` で拾える範囲。
- `rg` で参照が明確に1箇所だけのhelper。
- C unitからしか参照されない非static helper。

静的解析だけでは見つけにくいもの:

- PHP extension entrypoint、`PHP_FUNCTION`、`PHP_METHOD`、class registration経由の到達。
- nghttp2 callback、Zend resource destructor、object handler、function pointer経由の到達。
- macroやgenerated arginfoで間接参照されるsymbol。
- bench build限定、coverage build限定、diagnostic path限定のsymbol。
- struct fieldの「書いているが読んでいない」ケース。callbackやdebug outputで読まれる場合もある。
- public互換性のために残すconstantやmethod。C側から未使用でも削除できない。

## Proposed Approach

1. 現在のbuild flagsと警告設定を確認し、未使用symbol検出に使えるcompiler warningを整理する。
2. `rg` / `git grep` で関数定義と参照数を機械的に一覧化する。
3. `static` にできるもの、削除できるもの、PHP/Zend callbackとして残すものを分類する。
4. C unit / PHPT / bench build / diagnostic buildで到達差分を確認する。
5. 削除候補は1コミット1系統で扱い、必要ならissueを子分けする。

## Verification

調査のみ:

- `git diff --check`

削除または参照境界を変える場合:

- `git diff --check`
- `docker compose run --rm dev sh -lc 'make -j$(nproc)'`
- `./tools/test/check-c-static-analysis.sh`
- `./tools/test/check-c-unit.sh`
- `./tools/test/check-phpt.sh`

diagnostic/benchに触る場合:

- bench buildが必要なら該当build/testを追加する。
- runtime pathやhot pathに影響する削除では、影響するbenchをissueへ記録してから実行する。

## Non-Goals

- 未使用に見えるPHP-visible APIを互換性確認なしに削除しない。
- performance-sensitiveなhelper整理を、before/afterなしに最適化として扱わない。
- `metadata-exchange-split` のdomain boundary変更と混ぜない。
- 大規模なheader再編をこのissueへ混ぜない。

## Decision Log

- 2026-05-31: `hash_bytes()` が過去のproduction helperからtest-only残骸になっていたため、同種のdead codeを独立issueで調査する。
- 2026-05-31: 静的解析は有用だが、PHP extensionのentrypoint、callback、bench build、PHPT到達は静的解析だけでは判定しにくい。機械検出と手動分類を組み合わせる。

## Close Criteria

- dead code候補の一覧と分類が記録されている。
- 削除するもの、残すもの、別issue化するものの判断理由が記録されている。
- 実装変更を含む場合は必要な検証が通っている。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
