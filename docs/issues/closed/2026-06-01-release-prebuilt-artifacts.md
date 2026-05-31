# Release prebuilt grpc.so artifacts

- Status: Closed
- Created: 2026-06-01
- Branch: codex/release-prebuilt-assets
- Owner: Codex

## Background

PIE / source buildは主install経路として維持する。一方で、releaseごとに `grpc.so` をすぐ試せる導線があると、benchmark、diagnostic、実アプリでの差し替え確認が軽くなる。

既存の `ext-grpc-artifacts` は `<grpc-version>-php<php-version>-<distro>-<arch>-<profile>` のtag setでartifactを整理している。`php-grpc-lite` ではGitHub Release assetとして公開し、profileは付けず、代わりにPHP ABIとして `nts` / `zts` を明示する。

## Goals

- GitHub Release publish時にprebuilt `grpc.so` artifactをbuildし、release assetへuploadする。
- `workflow_dispatch` で同じartifact setを再作成・再uploadできるようにする。
- artifact variationは PHP 8.4 / 8.5、NTS / ZTS、amd64 / arm64、Debian trixieの8本にする。
- 各assetは `grpc.so`、metadata、checksumを含むtarballにする。

## Artifact Set

命名:

```text
php-grpc-lite-<version>-php<php-version>-<nts|zts>-trixie-<arch>.tar.gz
```

初期セット:

```text
php-grpc-lite-<version>-php8.4-nts-trixie-amd64.tar.gz
php-grpc-lite-<version>-php8.4-nts-trixie-arm64.tar.gz
php-grpc-lite-<version>-php8.4-zts-trixie-amd64.tar.gz
php-grpc-lite-<version>-php8.4-zts-trixie-arm64.tar.gz
php-grpc-lite-<version>-php8.5-nts-trixie-amd64.tar.gz
php-grpc-lite-<version>-php8.5-nts-trixie-arm64.tar.gz
php-grpc-lite-<version>-php8.5-zts-trixie-amd64.tar.gz
php-grpc-lite-<version>-php8.5-zts-trixie-arm64.tar.gz
```

tarball layout:

```text
artifacts/grpc.so
artifacts/metadata.json
SHA256SUMS
```

## Non-Goals

- PIE / Packagist installを置き換えない。
- GHCR container artifact publishingはこのissueでは行わない。
- `optimized-amd64-skylake` などCPU固有profileは初期セットに含めない。
- PHP 8.4未満は対象にしない。

## Plan

1. release artifact用Dockerfileを追加する。
2. matrix artifact build scriptを追加し、localとGitHub Actionsの両方から使えるようにする。
3. `release.published` / `workflow_dispatch` でassetをbuildし、GitHub Releaseへuploadするworkflowを追加する。
4. install guide / release QA checklistにprebuilt artifact導線を記録する。
5. local smokeで1 variationのtarball作成とmetadataを確認する。

## Progress

- 2026-06-01: issue作成。
- 2026-06-01: `Dockerfile.release-artifact` を追加。
- 2026-06-01: `tools/release/build-prebuilt-artifact.sh` を追加。localとGitHub Actionsの両方で1 variationのtarballを作る入口にする。
- 2026-06-01: `.github/workflows/release-prebuilt-artifacts.yml` を追加。`release.published` と `workflow_dispatch` で8 variationをbuildし、release assetへuploadする。
- 2026-06-01: install guideとrelease QA checklistへprebuilt artifact導線を追加。
- 2026-06-01: local smokeとして `0.0.12 / php8.4 / nts / trixie / arm64` のtarballをbuildし、layoutとmetadataを確認。

## Verification

- `bash -n tools/release/build-prebuilt-artifact.sh`: PASS
- `git diff --check`: PASS
- `./tools/release/build-prebuilt-artifact.sh 0.0.12 8.4 nts trixie arm64`: PASS
  - output: `dist/release-artifacts/php-grpc-lite-0.0.12-php8.4-nts-trixie-arm64.tar.gz`
  - tar layout:
    - `artifacts/grpc.so`
    - `artifacts/metadata.json`
    - `SHA256SUMS`
  - metadata:
    - `project`: `php-grpc-lite`
    - `version`: `0.0.12`
    - `php_version`: `8.4`
    - `php_version_full`: `8.4.21`
    - `thread_safety`: `nts`
    - `distro`: `trixie`
    - `arch`: `arm64`
    - `extension_version`: `0.0.12`

## Decision Log

- 2026-06-01: 公開先はGitHub Release assetにする。GHCR container packageは、Dockerfile内でartifactを `COPY --from` したい需要が強くなった段階で別issueにする。
- 2026-06-01: profile名は付けない。`ext-grpc-artifacts` の `pecl` に相当する通常buildだが、このprojectではsource buildなのでprofile axisを省く。
- 2026-06-01: NTS / ZTSはartifact名へ明示する。PHP extension directory名も `no-debug-non-zts-*` / `no-debug-zts-*` でABIが異なるため、利用者が誤って混ぜないようにする。
- 2026-06-01: workflowは `release.published` を主triggerにする。tag push時点ではReleaseが存在しない場合があるため、asset upload先が明確なrelease eventへ寄せる。失敗時や再作成用に `workflow_dispatch` も用意する。

## Close Criteria

- release artifact workflowが追加されている。
- 8 variationのmatrixがworkflowに定義されている。
- 1 variation以上をlocal smokeでbuildし、tarball layoutとmetadataを確認している。
- docsに取得方法と注意点が記録されている。
- `Status: Closed` にして `docs/issues/closed/` へ移動する。
