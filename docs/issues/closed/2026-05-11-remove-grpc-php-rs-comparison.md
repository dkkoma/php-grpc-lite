# remove grpc-php-rs comparison environment

Status: Closed

## 目的

現行で使わない `grpc-php-rs` one-shot比較環境と実行導線を削除し、Docker composeを通常の php-grpc-lite / ext-grpc 比較に絞る。

## 背景

`grpc-php-rs` 比較は過去の一度きりの調査用途で、現在の通常ベンチやCIには含めない。残っている `Dockerfile.grpc-php-rs` と `dev-grpc-rs` service は現行メンテ対象に見えるため削除する。

## スコープ

- `Dockerfile.grpc-php-rs` を削除する。
- `compose.yaml` の `dev-grpc-rs` serviceを削除する。
- 現行作業指示から `grpc-php-rs` 実行導線の参照を削除する。
- 過去の計測記録は履歴として残す。

## 非スコープ

- php-grpc-lite / ext-grpc の通常ベンチ変更。
- franken-go backend比較導線の変更。

## 計画

- 参照箇所を棚卸しする。
- Docker/compose/現行作業指示から `grpc-php-rs` 関連を削除する。
- 構文レベルの検証を行う。

## 進捗

- `Dockerfile.grpc-php-rs` を削除した。
- `compose.yaml` から `dev-grpc-rs` serviceを削除した。
- `AGENTS.md` から現行作業指示としての `grpc-php-rs` 再作成案内を削除した。
- `docs/benchmarks/comparison-grpc-php-rs-2026-04-27.md` は履歴として残し、当時の再実行導線であることを明示した。

## 検証

- `docker compose config --services`

## 判断ログ

- `grpc-php-rs` 比較は再利用する場合も現行runnerへ残さず、新しい専用runnerとして再作成する。

## 完了条件

- 現行コード・設定・作業指示に `grpc-php-rs` 比較環境が残らない。
- 過去の比較記録は現在の実行導線ではないことが分かる形で残る。
- compose設定が構文検証できる。

## 修正コミット

このコミット。
