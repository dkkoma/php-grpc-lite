# 0.0.13 release version alignment

Status: Closed
Target-Release: 0.0.13

## 目的

`0.0.13` releaseを作成する前に、package tag、extension runtime version、install検証例の期待値を同じversionとして揃える。

## 背景

`0.0.12` 以後、このprojectではrelease tagと `Grpc\VERSION` / `phpversion("grpc")` を一致させる方針にしている。GitHub Release assetのprebuilt `grpc.so` もmetadataへrelease versionとextension versionを書き出すため、tag作成前にruntime側のversionを先に更新する必要がある。

## スコープ

- `PHP_GRPC_VERSION` を `0.0.13` に更新する。
- README / install guide の現在release向け検証例を `0.0.13` に更新する。
- release tag / GitHub Release作成後、GitHub Actionsでprebuilt `grpc.so` assetsが生成されることを確認する。

## 非スコープ

- 過去releaseの履歴ドキュメントに残る `0.0.12` 以前の記録は変更しない。
- runtime挙動、transport実装、benchmark条件は変更しない。
- prebuilt artifact workflowのmatrixやbuild方法は、このissueでは変更しない。

## 計画

1. `PHP_GRPC_VERSION` とinstall検証例を `0.0.13` に揃える。
2. build/load smokeで `Grpc\VERSION === "0.0.13"` を確認する。
3. 変更をcommitし、mainへ取り込む。
4. `0.0.13` GitHub Releaseを作成し、prebuilt release assets生成を確認する。

## 進捗

- 2026-06-01: issue作成。
- 2026-06-01: `PHP_GRPC_VERSION` と現在release向けinstall検証例を `0.0.13` に更新。
- 2026-06-01: `0.0.13` GitHub Releaseを作成。
- 2026-06-01: 修正済みrelease artifact workflowを `workflow_dispatch` / `tag=0.0.13` で再実行し、release assets添付を確認。

## 検証

- `git diff --check`: PASS
- `bash -n tools/release/build-prebuilt-artifact.sh`: PASS
- `docker compose run --rm dev sh -lc 'phpize && ./configure --enable-grpc && make -j$(nproc) && php -d extension=/workspace/modules/grpc.so -r ...'`: PASS
  - `extension_loaded("grpc")`: `true`
  - `Grpc\VERSION === "0.0.13"`: `true`
  - `phpversion("grpc")`: `0.0.13`
- `gh release view 0.0.13 --repo dkkoma/php-grpc-lite --json tagName,targetCommitish,url,assets`: PASS
  - release URL: `https://github.com/dkkoma/php-grpc-lite/releases/tag/0.0.13`
  - assets: 8 tarballs + `SHA256SUMS`
- `gh run watch 26728548774 --repo dkkoma/php-grpc-lite --exit-status`: PASS
  - build matrix: php8.4/php8.5 x nts/zts x amd64/arm64
  - publish job: PASS
- representative asset inspection:
  - asset: `php-grpc-lite-0.0.13-php8.4-nts-trixie-arm64.tar.gz`
  - layout: `artifacts/grpc.so`, `artifacts/metadata.json`, `SHA256SUMS`
  - `metadata.version`: `0.0.13`
  - `metadata.release_tag`: `0.0.13`
  - `metadata.thread_safety`: `nts`
  - `metadata.arch`: `arm64`
  - `metadata.extension_version`: `0.0.13`
  - `metadata.source_sha`: `f6b6612514c0b3012521eddb3774510af29f2898`

## 判断ログ

- `0.0.13` releaseでは、runtime versionが `0.0.12` のままになる状態を避けるため、tag作成前にversion alignmentを独立commitとして扱う。
- 初回 `release.published` run `26727833084` は、8本のbuild matrixは成功したがpublish jobで失敗した。原因は別issue `2026-06-01-release-asset-upload-repo-context.md` で修正した。

## 修正コミット

- `36741b3` `0.0.13 release: runtime versionをtagと揃える`
- `f6b6612` `Merge branch 'codex/release-0-0-13-version'`

## 完了条件

- `PHP_GRPC_VERSION` が `0.0.13`。
- README / install guide の現在release向け検証例が `0.0.13`。
- build/load smokeで `Grpc\VERSION === "0.0.13"` を確認済み。
- `0.0.13` GitHub Releaseにrelease assetsが添付されている。
