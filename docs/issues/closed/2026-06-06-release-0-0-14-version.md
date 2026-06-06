# 0.0.14 release version alignment

Status: Closed
Target-Release: 0.0.14
Branch: codex/tighten-symbol-visibility

## 目的

symbol visibility tightening を含む現在のブランチ先端を `0.0.14` release候補として扱えるよう、runtime versionと現在release向けinstall検証例を `0.0.14` に揃える。

## 背景

`0.0.13` release後に symbol visibility制限を追加した。tag作成前に `PHP_GRPC_VERSION` とREADME / install guideの確認例を次release番号へ揃え、runtime versionが前releaseのままになる状態を避ける。

## スコープ

- `PHP_GRPC_VERSION` を `0.0.14` に更新する。
- README / install guide の現在release向け検証例を `0.0.14` に更新する。
- Docker compose内で build/load smokeを確認する。

## 非スコープ

- GitHub Release作成。
- prebuilt artifact生成。
- 過去の `0.0.13` release issue / review記録の書き換え。

## 計画

1. runtime version macroとinstall検証例を `0.0.14` に揃える。
2. build/load smokeで `Grpc\VERSION === "0.0.14"` を確認する。
3. issueに検証結果と修正コミットを記録してcloseする。

## 進捗

- 2026-06-06: issue作成。
- 2026-06-06: `PHP_GRPC_VERSION` と README / install guide の現在release向け検証例を `0.0.14` に更新。
- 2026-06-06: Docker compose内で build/load smokeを確認。

## 検証

- build/load smoke: PASS
  - command: `docker compose run --rm dev sh -lc 'cd /workspace && make clean >/tmp/grpc-0.0.14-clean.log 2>&1 || true; rm -rf .libs modules *.lo *.o *.dep src/.libs src/*.lo src/*.o src/*.dep src/diagnostic/.libs src/diagnostic/*.lo src/diagnostic/*.o src/diagnostic/*.dep; phpize >/tmp/grpc-0.0.14-phpize.log; ./configure --enable-grpc >/tmp/grpc-0.0.14-configure.log; make -j$(nproc) >/tmp/grpc-0.0.14-make.log; php -d extension=/workspace/modules/grpc.so -r '\''var_dump(extension_loaded("grpc"), Grpc\VERSION, phpversion("grpc")); exit(extension_loaded("grpc") && Grpc\VERSION === "0.0.14" && phpversion("grpc") === "0.0.14" ? 0 : 1);'\'''`
  - `extension_loaded("grpc")`: `true`
  - `Grpc\VERSION`: `0.0.14`
  - `phpversion("grpc")`: `0.0.14`

## 判断ログ

- 2026-06-06: 過去releaseの検証記録として残る `docs/issues/closed/*0.0.13*` は履歴なので更新しない。

## 修正コミット

- `3b8dd44` `0.0.14 release: runtime versionをtagと揃える`

## 完了条件

- `PHP_GRPC_VERSION` が `0.0.14`。
- README / install guide の現在release向け検証例が `0.0.14`。
- build/load smokeで `Grpc\VERSION === "0.0.14"` を確認済み。
- issueを `Status: Closed` に更新し、修正コミットと検証結果を記録して `docs/issues/closed/` へ移動する。
