# ext-grpc artifact image repository 指示書

## 目的

`php-grpc-lite` のベンチマークや互換検証で使う公式 `ext-grpc` を、毎回 `pecl install grpc` でビルドせずに再利用できるようにする。

別リポジトリで `grpc.so` だけを含む artifact container image を GitHub Container Registry にpublishし、利用側のDockerfileでは `COPY --from=... /artifacts/grpc.so` だけで公式 `ext-grpc` を組み込む。

この仕組みは `php-grpc-lite` 本体リポジトリには混ぜない。公式 `ext-grpc` artifact のビルド・保存・更新責務は `dkkoma/ext-grpc-artifacts` に分離する。

## リポジトリの役割

- `grpc.so` artifact image をビルドしてGHCRへpublishする。
- 複数の `grpc` version、PHP ABI、CPU architecture、build profile をmatrixで管理する。
- 公式 `grpc` の新バージョン公開時に、必要なartifact imageを追加で焼けるようにする。
- artifact imageはruntime app imageではない。`grpc.so`、build metadata、license表記だけを持つ。

## 対象matrix

### grpc version

- `1.58.0`
- `1.80.0`

将来は公式 `grpc` の新バージョン公開を契機に追加する。

### PHP ABI

- PHP `8.4`
- PHP `8.5`

PHP拡張のABIディレクトリが異なるため、PHP minor versionごとに別artifactを作る。

### architecture

- `linux/amd64`
- `linux/arm64`

### base distro

- `trixie`

対象version `1.58.0` / `1.80.0` では `trixie` に固定する。将来versionでbase distroを増やすかどうかは、その時点のPHP公式image、grpc source、toolchain事情を見て判断する。

### build profile

#### `pecl`

通常の `pecl install grpc-${GRPC_VERSION}` 相当。

目的:

- 利用者が普通にPECLから入れる場合に近い比較対象を用意する。
- optimized profileとの差分を明確にする。
- `amd64` / `arm64` の両方でpublishする。

#### `optimized-amd64-skylake`

`amd64` 向けの最適化ビルド。`arm64` にはpublishしない。

初期条件:

- compiler: `gcc-15`
- `CFLAGS`: `-O3 -flto -fno-semantic-interposition -march=skylake`
- `CXXFLAGS`: `-O3 -flto -fno-semantic-interposition -march=skylake`
- `LDFLAGS`: `-flto`
- target: x86-64 / Skylake 想定

`grpc 1.58.0` は `gcc-15` / 新しめのC++ toolchainで `cstdint` include不足に当たる可能性があるため、必要な場合だけ `CXXFLAGS` に以下を追加する。

```sh
-include cstdint
```

将来的にはLLVM/Clang profileも追加候補にする。ただし初期スコープには含めない。

## GHCR image設計

image repository:

```text
ghcr.io/dkkoma/ext-grpc-artifacts
```

tag形式:

```text
<grpc-version>-php<php-version>-<distro>-<arch>-<profile>
```

代表tag:

```text
ghcr.io/dkkoma/ext-grpc-artifacts:1.58.0-php8.4-trixie-amd64-pecl
ghcr.io/dkkoma/ext-grpc-artifacts:1.58.0-php8.4-trixie-amd64-optimized-amd64-skylake
ghcr.io/dkkoma/ext-grpc-artifacts:1.80.0-php8.5-trixie-arm64-pecl
```

multi-arch manifest tagは必須ではない。利用側がPHP ABIとarchを明示して `COPY --from` する用途なので、arch込みtagを正とする。

## artifact imageの中身

配置:

```text
/artifacts/grpc.so
/artifacts/metadata.json
/licenses/APACHE-2.0.txt
/licenses/NOTICE
```

`metadata.json` には最低限以下を入れる。

```json
{
  "grpc_version": "1.58.0",
  "php_version": "8.4",
  "php_extension_dir": "no-debug-non-zts-20240924",
  "distro": "trixie",
  "arch": "amd64",
  "profile": "optimized-amd64-skylake",
  "compiler": "gcc-15",
  "cflags": "-O3 -flto -fno-semantic-interposition -march=skylake",
  "cxxflags": "-O3 -flto -fno-semantic-interposition -march=skylake -include cstdint",
  "ldflags": "-flto",
  "built_at": "ISO-8601 timestamp",
  "source": "pecl grpc-1.58.0"
}
```

利用側では以下のように使う。

```dockerfile
FROM ghcr.io/dkkoma/ext-grpc-artifacts:1.58.0-php8.4-trixie-amd64-optimized-amd64-skylake AS ext-grpc
FROM php:8.4-fpm-trixie

COPY --from=ext-grpc /artifacts/grpc.so /tmp/grpc.so
RUN cp /tmp/grpc.so "$(php-config --extension-dir)/grpc.so" \
 && echo "extension=grpc.so" > /usr/local/etc/php/conf.d/docker-php-ext-grpc.ini
```

PHP extension dirはPHP buildごとに変わるため、利用側Dockerfileでは `php-config --extension-dir` または `ini_get("extension_dir")` で配置先を決める。

## GitHub Actions設計

### trigger

- `workflow_dispatch`
- `discover-release` workflowによる新version検知

### matrix

```yaml
grpc-version: ["1.58.0", "1.80.0"]
php-version: ["8.4", "8.5"]
arch: ["amd64", "arm64"]
profile: ["pecl", "optimized-amd64-skylake"]
distro: ["trixie"]
```

ただし `optimized-amd64-skylake` は `amd64` のみpublishする。

### build

- `docker/setup-buildx-action` を使う。
- `docker/build-push-action` でGHCRへpushする。
- `platforms` はmatrixのarchに合わせて単独指定する。
- build cacheはGitHub Actions cacheを使う。
- `optimized-amd64-skylake` profileは `gcc-15` をDockerfile内で明示的に使う。

## 検証ゲート

artifact imageごとに最低限以下を確認する。

```sh
php -d extension=/artifacts/grpc.so -m | grep '^grpc$'
php -d extension=/artifacts/grpc.so --ri grpc
```

可能なら、同じPHP base imageにCOPYして以下も確認する。

```sh
php -m | grep '^grpc$'
php -r 'var_dump(class_exists("Grpc\\Channel"));'
```

このartifact repoの責務は `grpc.so` がロードできることまで。`php-grpc-lite` のLaravel/FPM Spanner benchや互換検証は本体リポジトリ側で行う。

## php-grpc-lite側での利用方針

`php-grpc-lite` 側で公式 `ext-grpc` を比較対象として使う場合は、特にパッチやカスタムinstrumentationが必要ない限り `ghcr.io/dkkoma/ext-grpc-artifacts:<tag>` から `grpc.so` をCOPYする。

- Laravel/FPM bench image、Spanner repro image、今後追加するdiagnostic imageの通常official variantでは `pecl install grpc-${version}` をしない。
- frame traceなど公式 `ext-grpc` 自体へpatchを当てる必要があるdiagnostic targetだけsource buildを許可する。
- amd64のperformance comparatorでは `optimized-amd64-skylake` を優先する。
- arm64や最適化条件を揃えない互換確認では `pecl` を使う。
- artifact tagは `<grpc-version>-php<php-version>-<distro>-<arch>-<profile>` を使う。
- `Dockerfile` 側では `EXT_GRPC_ARTIFACT_ARCH` build argでartifact archを明示できるようにする。GitHub Actions / GCP VMのamd64比較では `amd64`、arm64ローカル確認では `arm64` + `pecl` を指定する。

これにより、VM上はもちろんGitHub Actions上でもofficial image build時間とbuild artifact差を削減する。

## 完了条件

- `1.58.0` / `1.80.0`
- PHP `8.4` / `8.5`
- `amd64` / `arm64`
- `pecl` all arch
- `optimized-amd64-skylake` amd64 only
- `trixie`

上記のGHCR tagがpublishされる。

各tagについて `php -d extension=/artifacts/grpc.so -m` が成功する。

## 注意点

- このrepoは公式 `ext-grpc` の再配布artifactを作るため、license表記とsource情報をREADMEに明記する。
- artifact imageには秘密情報を含めない。
- `grpc 1.58.0` の `-include cstdint` は必要な場合だけ適用する。不要なversionに無条件で入れない。
- optimized profileの性能差は利用側ベンチで評価する。artifact repoではロード可能性とbuild再現性を主目的にする。
- 将来、PHP公式imageのbase distroやPHP ABIが変わった場合はtag体系を壊さず、新しいdistro/profileを追加する。
