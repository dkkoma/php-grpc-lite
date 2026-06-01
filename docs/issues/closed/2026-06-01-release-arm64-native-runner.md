# Release arm64 native runner

Status: Closed
Target-Release: 0.0.13

## 目的

prebuilt `grpc.so` release artifactのarm64 buildをQEMU emulationではなく、GitHub-hosted arm64 native runnerで実行する。

## 背景

`0.0.13` release artifact workflowは8種類のtarball生成に成功したが、arm64 buildも `ubuntu-24.04` amd64 runner上でQEMUを使っていた。artifactとしては正しく生成できるものの、release artifactの標準buildではnative runnerを使う方がよい。

理由:

- arm64 targetのcompiler / linker / Docker base image実行をnative環境で確認できる。
- QEMU由来の遅さや不安定さをrelease buildから外せる。
- amd64 / arm64 それぞれのrunner architectureとartifact architectureが一致し、検証意図が読みやすくなる。

GitHub-hosted runnerでは Linux arm64 labelとして `ubuntu-24.04-arm` が提供されているため、public repositoryではこのlabelを使える。

## スコープ

- matrixの `arch` に応じて `runs-on` を選ぶ。
  - `amd64`: `ubuntu-24.04`
  - `arm64`: `ubuntu-24.04-arm`
- QEMU setup stepを削除する。
- `0.0.13` releaseに対してworkflowを再実行し、release assetsをnative arm64 buildで上書きする。

## 非スコープ

- artifact名、tarball layout、metadata schemaは変更しない。
- `0.0.13` tagの指し先は変更しない。
- Dockerfile / build scriptのbuild contractは変更しない。

## 計画

1. release artifact workflowのrunner選択をarch別にする。
2. QEMU setupを削除する。
3. YAML構文とdiffを確認する。
4. mainへ取り込み、`workflow_dispatch` / `tag=0.0.13` で再実行する。
5. release assetsと代表arm64 assetのmetadataを確認する。

## 進捗

- 2026-06-01: issue作成。
- 2026-06-01: `runs-on` をarch別にし、QEMU setup stepを削除。
- 2026-06-01: mainへ取り込み、`workflow_dispatch` / `tag=0.0.13` でrelease artifact workflowを再実行。
- 2026-06-01: `0.0.13` release assetsをnative arm64 buildで再生成。

## 検証

- `ruby -e 'require "yaml"; YAML.load_file(".github/workflows/release-prebuilt-artifacts.yml"); puts "yaml ok"'`: PASS
- `git diff --check`: PASS
- `gh run watch 26729366977 --repo dkkoma/php-grpc-lite --exit-status`: PASS
  - build matrix: php8.4/php8.5 x nts/zts x amd64/arm64
  - publish job: PASS
- representative arm64 job log:
  - job: `Build php8.4 nts arm64`
  - job id: `78770243689`
  - VM Image: `Linux (arm64)`
  - image name: `Ubuntu 24.04 by Arm Limited`
  - Docker client/server `OS/Arch`: `linux/arm64`
  - QEMU setup step: absent
- `gh release view 0.0.13 --repo dkkoma/php-grpc-lite --json url,assets`: PASS
  - assets: 8 tarballs + `SHA256SUMS`
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

- Docker Buildxは引き続き使う。`docker buildx build --platform linux/<arch>` によりartifact architectureは明示しつつ、runner architectureと一致する場合はQEMUなしでbuildできる。
- `0.0.13` tagのsourceは変更せず、main上のworkflowだけを更新して既存release assetsを `--clobber` で再生成した。

## 修正コミット

- `87ce7df` `Release artifact: arm64 buildをnative runnerにする`
- `3a61d03` `Merge branch 'codex/release-arm64-native-runner'`

## 完了条件

- arm64 build jobが `ubuntu-24.04-arm` で実行されている。
- amd64 build jobが `ubuntu-24.04` で実行されている。
- `0.0.13` release assetsが再生成されている。
