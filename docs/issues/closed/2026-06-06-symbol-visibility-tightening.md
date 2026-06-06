# 2026-06-06 symbol visibility tightening

Status: Closed
Branch: codex/tighten-symbol-visibility

## 目的

PHP extension の dynamic symbol export を loader に必要な範囲へ絞り、internal C helper や `PHP_METHOD` 実体が shared object 外へ見える状態を解消する。

## 背景

multi translation unit 化後、`modules/grpc.so` では `nm -D --defined-only` 上で 135 個の定義済み dynamic symbol が見えている。`get_module` 以外に transport / protocol / TLS helper、`grpc_ce_*`、`le_server_streaming_call_state`、`zim_*` method handler も default visibility で export されている。

PHP userland API と C dynamic ABI は別物であり、method table から関数ポインタで参照される `PHP_METHOD` 実体は shared object 外へ公開する必要がない。不要 export は他 extension や embedder との symbol collision / interposition のリスクを増やす。

## スコープ

- GCC/Clang build で extension 本体を hidden visibility にする。
- PHP loader に必要な `get_module` が引き続き default visibility で export されることを確認する。
- 必要に応じて `grpc_module_entry` の扱いを明確にする。
- `nm -D --defined-only modules/grpc.so` の allowlist 検証を追加する。
- Docker compose 内で build/load と C/PHPT の代表検証を行う。

## 非スコープ

- Windows / `config.w32` 対応。
- transport / protocol の責務分割や header 再設計。
- hotpath 最適化としての inline / LTO 変更。
- ext-grpc との性能比較。

## 計画

1. 現状の exported symbol を再確認する。
2. `config.m4` に compiler visibility capability check と `-fvisibility=hidden` を追加する。
3. 必要な public symbol を最小化する。
4. symbol allowlist check script を追加し、test runner から呼べるようにする。
5. Docker compose 内で build/load、symbol allowlist、C unit、PHPT を確認する。
6. issue に検証結果と判断ログを追記して close する。

## 進捗

- 2026-06-06: issue 作成。
- 2026-06-06: `config.m4` に `-fvisibility=hidden` capability checkを追加し、production / bench buildの dynamic export allowlist checkを追加した。
- 2026-06-06: Docker compose内で symbol visibility、C unit、C static analysis、PHPT、ZTS PHPTを確認した。

## 検証

- `./tools/test/check-symbol-visibility.sh`: PASS
  - production build: `nm -D --defined-only modules/grpc.so` は `get_module` のみ。
  - bench build: `nm -D --defined-only modules/grpc.so` は `get_module` のみ。
- `docker compose run --rm --no-deps dev sh -lc 'cd /workspace && grep -n "fvisibility=hidden\|GRPC_VISIBILITY" Makefile Makefile.objects config.log | head -40 && nm -D --defined-only modules/grpc.so'`: PASS
  - generated Makefile / Makefile.objects に `-fvisibility=hidden` が入り、`nm` は `get_module` のみを表示。
- `./tools/test/check-c-unit.sh`: PASS
  - `protocol_core unit tests passed`
  - `status_core unit tests passed`
  - `transport_core unit tests passed`
- `./tools/test/check-c-static-analysis.sh`: PASS
- `git diff --check`: PASS
- `./tools/test/check-phpt.sh`: PASS, 15/15
- `./tools/test/check-zts-phpt.sh`: PASS, 15/15

## 判断ログ

- 2026-06-06: PHP 8.4 header では `ZEND_GET_MODULE` が `ZEND_DLEXPORT` 付きで `get_module` を生成することを確認した。`PHP_METHOD` / `ZEND_METHOD` 自体には export 属性がないため、hidden visibility にしても method table 経由の呼び出しは成立する前提で進める。
- 2026-06-06: `grpc_module_entry` は `get_module()` が同一shared object内で返せば足りるため、dynamic export allowlistには入れない。allowlistは `get_module` のみにする。
- 2026-06-06: compilerが `-fvisibility=hidden` を受け付けない場合は configure failure とする。allowlist checkの目的と一致させ、広いsymbol exportでのfallback buildは許可しない。

## 修正コミット

- `c51647e` C拡張のsymbol visibilityを制限

## 完了記録

- 終了: 2026-06-06 JST
- production / bench build ともに dynamic export は `get_module` のみに制限済み。
- `tools/test/check-native-release-hardening.sh` に symbol visibility gateを追加済み。

## 完了条件

- production build の `modules/grpc.so` で不要な internal helper / `zim_*` が dynamic export されない。
- PHP extension load と既存テストが通る。
- symbol allowlist check が追加され、今後の accidental export を検出できる。
- issue を `Status: Closed` に更新し、検証結果と修正コミットを記録して `docs/issues/closed/` へ移動する。
