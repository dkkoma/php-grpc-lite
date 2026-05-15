---
Status: Closed
Owner: Codex
Created: 2026-05-15
Branch: main
---

# タグ別grpc.soをベンチで再利用しやすくする

## 目的

リリースタグごとの `grpc.so` を現在のworking treeを壊さず取得し、real application benchmarkで差し替えられるようにする。

## 背景

0.0.3と0.0.2のruntime差分をreal Spanner + Laravel/FPM経路で比較するには、PHP userlandやベンチrunnerは現在のまま、native extension binaryだけタグごとに切り替えられる必要がある。

## スコープ

- git tagから `ext/grpc` を別ディレクトリに展開して `grpc.so` をビルドする。
- 成果物を `var/tag-so/<tag>/grpc.so` に保存する。
- FPM native serviceで読み込む `grpc.so` pathを環境変数で差し替えられるようにする。

## 非スコープ

- タグごとのPHP userland全体を実行すること。
- リリースartifact配布方式を変更すること。

## 実装

- `tools/dev/build-tag-so.sh <tag>` を追加した。
- `NATIVE_GRPC_SO` でFPM native serviceのextension pathを差し替えられるようにした。

## 使い方

```bash
./tools/dev/build-tag-so.sh 0.0.2
NATIVE_GRPC_SO=/workspace/var/tag-so/0.0.2/grpc.so \
  BENCH_ACTIONS='select_1row_10col dml_insert_10col' \
  LARAVEL_SPANNER_EMULATOR_HOST= \
  ./bench/fpm-laravel-spanner-load-compare.sh 128 4
```

## 検証

- 手動で0.0.2 tagを展開し、`var/tag-so/0.0.2/grpc.so` をビルドした。
- NATIVE_GRPC_SOで0.0.2のsoを読み込ませ、real Spanner + Laravel/FPM single select / single DML benchmarkを実行した。

## 完了条件

- タグ別soをcurrent working treeと分離して取得できる。
- native FPM benchmarkでso pathを差し替えられる。

## 完了

- 完了条件を満たしたためClosed。
