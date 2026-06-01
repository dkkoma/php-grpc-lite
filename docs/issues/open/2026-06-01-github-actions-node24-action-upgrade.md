# GitHub Actions Node 24 action upgrade

Status: Open
Target-Release: 0.0.13 follow-up

## 目的

GitHub Actions上の Node.js 20 deprecation warningを解消するため、workflowで使っているofficial / Docker actionsをNode 24 runtime対応版へ更新する。

## 背景

`0.0.13` release artifact workflowの実行時に、`actions/checkout@v4`、`actions/upload-artifact@v4`、`actions/download-artifact@v4`、`docker/setup-buildx-action@v3` がNode.js 20 deprecation warningを出していた。

GitHub-hosted runnerではNode.js 24への移行が進んでおり、release workflowの将来の安定性を考えると、warningを放置しない方がよい。

調査結果:

- `actions/checkout@v5` はNode 24 runtime対応。
- `actions/upload-artifact@v6` はNode 24 runtime対応。
- `actions/download-artifact@v7` はNode 24 runtime対応。
- `docker/setup-buildx-action@v4` はNode 24 runtime対応。

## スコープ

- `.github/workflows/release-prebuilt-artifacts.yml`
- `.github/workflows/native-qa.yml`
- `.github/workflows/publish-diagnostic-images.yml`

対象更新:

- `actions/checkout@v4` -> `actions/checkout@v5`
- `actions/upload-artifact@v4` -> `actions/upload-artifact@v6`
- `actions/download-artifact@v4` -> `actions/download-artifact@v7`
- `docker/setup-buildx-action@v3` -> `docker/setup-buildx-action@v4`

## 非スコープ

- workflowのjob構成、matrix、cache scope、release asset名は変更しない。
- release tagの指し先は変更しない。
- third-party actionsでwarningが出ていないものはこのissueでは変更しない。

## 計画

1. 対象action versionだけを更新する。
2. YAML構文とdiffを確認する。
3. release artifact workflowを `workflow_dispatch` / `tag=0.0.13` で再実行する。
4. warningが消え、release assetsが維持されることを確認する。

## 進捗

- 2026-06-01: issue作成。
- 2026-06-01: 対象workflowのaction versionをNode 24対応版へ更新。

## 検証

- `ruby -e 'require "yaml"; Dir[".github/workflows/*.yml"].each { |f| YAML.load_file(f) }; puts "yaml ok"'`: PASS
- `git diff --check`: PASS
- `rg -n "actions/checkout@v4|actions/upload-artifact@v4|actions/download-artifact@v4|docker/setup-buildx-action@v3" .github/workflows`: no matches

## 判断ログ

- `actions/checkout` は最新の `v6` も存在するが、credential保存位置変更の挙動差分をこのissueに混ぜないため、Node 24対応目的として `v5` に留める。
- artifact actionsはNode 24がdefault runtimeになるversionへ上げるため、`upload-artifact@v6` / `download-artifact@v7` を使う。

## 完了条件

- release artifact workflowでNode.js 20 deprecation warningが出ない。
- `0.0.13` release assetsが引き続き8 tarballs + `SHA256SUMS` として存在する。
