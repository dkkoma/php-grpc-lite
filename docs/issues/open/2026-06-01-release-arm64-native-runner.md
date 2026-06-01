# Release arm64 native runner

Status: Open
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

## 検証

- `ruby -e 'require "yaml"; YAML.load_file(".github/workflows/release-prebuilt-artifacts.yml"); puts "yaml ok"'`: PASS
- `git diff --check`: PASS

## 判断ログ

- Docker Buildxは引き続き使う。`docker buildx build --platform linux/<arch>` によりartifact architectureは明示しつつ、runner architectureと一致する場合はQEMUなしでbuildできる。

## 完了条件

- arm64 build jobが `ubuntu-24.04-arm` で実行されている。
- amd64 build jobが `ubuntu-24.04` で実行されている。
- `0.0.13` release assetsが再生成されている。
