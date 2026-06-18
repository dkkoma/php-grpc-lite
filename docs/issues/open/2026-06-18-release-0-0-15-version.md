# 0.0.15 release version alignment

Status: Open
Target-Release: 0.0.15
Branch: main

## 目的

0.0.14 以後の transport hotpath改善と互換性修正を含む `main` 先端を `0.0.15` release候補として扱えるよう、runtime versionと現在release向けinstall検証例を `0.0.15` に揃える。

## 背景

`0.0.14` release後に unary response direct decode、request送信NO_COPY、write coalescing、metadata互換性修正などが `main` に取り込まれた。tag作成前に `PHP_GRPC_VERSION` と README / install guide の確認例を次release番号へ揃え、runtime versionが前releaseのままになる状態を避ける。

## スコープ

- `PHP_GRPC_VERSION` を `0.0.15` に更新する。
- README / install guide の現在release向け検証例を `0.0.15` に更新する。
- Docker compose内で build/load smokeを確認する。
- GitHub Release `0.0.15` を作成する。

## 非スコープ

- prebuilt artifact生成workflowの手動修正。
- 過去release issue / review記録の書き換え。
- 0.0.14以後にmerge済みの機能変更の追加実装。

## 計画

1. runtime version macroとinstall検証例を `0.0.15` に揃える。
2. build/load smokeで `Grpc\VERSION === "0.0.15"` を確認する。
3. 0.0.14からの差分をrelease noteへまとめる。
4. issueに検証結果と修正コミットを記録してcloseする。
5. tag / GitHub Release `0.0.15` を作成する。

## 進捗

- 2026-06-18: issue作成。

## 検証

- 未実施。

## 判断ログ

- 2026-06-18: 過去releaseの検証記録として残る `docs/issues/closed/*0.0.14*` は履歴なので更新しない。

## 修正コミット

- 未作成。

## 完了条件

- `PHP_GRPC_VERSION` が `0.0.15`。
- README / install guide の現在release向け検証例が `0.0.15`。
- build/load smokeで `Grpc\VERSION === "0.0.15"` を確認済み。
- GitHub Release `0.0.15` が作成済み。
- issueを `Status: Closed` に更新し、修正コミットと検証結果を記録して `docs/issues/closed/` へ移動する。
