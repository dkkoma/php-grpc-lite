# Release asset upload repository context

Status: Closed
Target-Release: 0.0.13

## 目的

prebuilt `grpc.so` artifact workflowのpublish jobが、checkoutなしでもGitHub Releaseへassetを添付できるようにする。

## 背景

`0.0.13` release作成で `release-prebuilt-artifacts.yml` は8本のbuild matrixをすべて成功させたが、最後の `Attach artifacts to release` stepだけ失敗した。ログでは `gh release upload` がrepository contextを解決しようとして `fatal: not a git repository` になっていた。

publish jobはartifactをdownloadしてreleaseへuploadするだけなので、source checkoutは必須ではない。`gh release upload` にrepositoryを明示すれば、`.git` の有無に依存しない。

## スコープ

- `gh release upload` に `--repo dkkoma/php-grpc-lite` を追加する。
- `0.0.13` releaseに対して `workflow_dispatch` で再実行し、release assetsが添付されることを確認する。

## 非スコープ

- artifactのbuild matrix、asset名、tarball layoutは変更しない。
- `0.0.13` tagの指し先は変更しない。

## 計画

1. release asset upload stepにrepositoryを明示する。
2. workflow YAMLを構文確認する。
3. mainへ取り込み、`workflow_dispatch` で `tag=0.0.13` を指定して再実行する。
4. GitHub Release assetsを確認する。

## 進捗

- 2026-06-01: issue作成。
- 2026-06-01: `gh release upload` に `--repo dkkoma/php-grpc-lite` を追加。
- 2026-06-01: 修正をmainへ取り込み、`0.0.13` releaseに対してworkflowを再実行。

## 検証

- `ruby -e 'require "yaml"; YAML.load_file(".github/workflows/release-prebuilt-artifacts.yml"); puts "yaml ok"'`: PASS
- `git diff --check`: PASS
- failed run確認:
  - run: `26727833084`
  - build matrix: PASS
  - publish job: FAIL
  - failed step: `Attach release assets`
  - error: `failed to run git: fatal: not a git repository`
- fixed run確認:
  - run: `26728548774`
  - event: `workflow_dispatch`
  - input tag: `0.0.13`
  - build matrix: PASS
  - publish job: PASS
- `gh release view 0.0.13 --repo dkkoma/php-grpc-lite --json tagName,targetCommitish,url,assets`: PASS
  - assets: 8 tarballs + `SHA256SUMS`

## 判断ログ

- publish jobにcheckoutを追加する案もあるが、uploadに必要なのはrepository指定だけなので、Git checkoutを増やさず `--repo` を明示する。

## 修正コミット

- `83f87f5` `Release artifact: upload先repositoryを明示`
- `5344e24` `Merge branch 'codex/fix-release-asset-upload-repo'`

## 完了条件

- `0.0.13` releaseに8個のprebuilt tarballと `SHA256SUMS` が添付されている。
- workflowのpublish jobが成功している。
