# ext-grpc artifact image repository 指示書

## 目的

`php-grpc-lite` のベンチマークや互換検証で使う公式 `ext-grpc` を、毎回 `pecl install grpc` でビルドせずに再利用できるようにする。

別リポジトリで `grpc.so` だけを含む artifact container image を GitHub Container Registry にpublishし、利用側のDockerfileでは `COPY --from=... grpc.so` だけで公式 `ext-grpc` を組み込める状態にする。

この仕組みは `php-grpc-lite` 本体リポジトリには混ぜない。公式 `ext-grpc` artifact のビルド・保存・更新責務を分離する。

## リポジトリの役割

- `grpc.so` artifact image をビルドしてGHCRへpublishする。
- 複数の `grpc` version、PHP ABI、CPU architecture、build profile をmatrixで管理する。
- 公式 `grpc` の新バージョン公開時に、必要なartifact imageを追加で焼けるようにする。
- artifact imageはruntime app imageではない。基本的には `grpc.so` と、必要ならbuild metadataだけを持つ。

## 初期対象matrix

### grpc version

- `1.58.0`
- `1.80.0`

将来は公式 `grpc` の新バージョン公開をhookにして追加する。

### PHP ABI

- PHP `8.4`
- PHP `8.5`

PHP拡張のABIディレクトリが異なるため、PHP minor versionごとに別artifactを作る。

### architecture

- `linux/amd64`
- `linux/arm64`

### base distro

初期は `trixie` に固定する。

対象version `1.58.0` / `1.80.0` では `trixie` でよい。将来versionでbase distroを増やすかどうかは、その時点のPHP公式image、grpc source、toolchain事情を見て判断する。

### build profile

#### `pecl`

通常の `pecl install grpc-${GRPC_VERSION}` 相当。

目的:

- 利用者が普通にPECLから入れる場合に近い比較対象を用意する。
- optimized profileとの差分を明確にする。

#### `optimized`

最適化ビルド。

初期条件:

- compiler: `gcc-15`
- `CFLAGS`: `-O3 -flto -fno-semantic-interposition`
- `CXXFLAGS`: `-O3 -flto -fno-semantic-interposition`
- `LDFLAGS`: `-flto`

`grpc 1.58.0` は `gcc-15` / 新しめのC++ toolchainで `cstdint` include不足に当たる可能性があるため、必要なら `CXXFLAGS` に以下を追加する。

```sh
-include cstdint
```

将来的にはLLVM/Clang profileも追加候補にする。ただし初期スコープには含めない。

## GHCR image設計

image repository例:

```text
ghcr.io/dkkoma/ext-grpc-artifacts
```

tag形式:

```text
<grpc-version>-php<php-version>-<distro>-<arch>-<profile>
```

初期tag例:

```text
ghcr.io/dkkoma/ext-grpc-artifacts:1.58.0-php8.4-trixie-amd64-pecl
ghcr.io/dkkoma/ext-grpc-artifacts:1.58.0-php8.4-trixie-amd64-optimized
ghcr.io/dkkoma/ext-grpc-artifacts:1.80.0-php8.5-trixie-arm64-pecl
ghcr.io/dkkoma/ext-grpc-artifacts:1.80.0-php8.5-trixie-arm64-optimized
```

multi-arch manifest tagは必須ではない。利用側がPHP ABIとarchを明示して `COPY --from` する用途なので、まずはarch込みtagを正とする。

## artifact imageの中身

推奨配置:

```text
/artifacts/grpc.so
/artifacts/metadata.json
```

`metadata.json` には最低限以下を入れる。

```json
{
  "grpc_version": "1.58.0",
  "php_version": "8.4",
  "php_extension_dir": "no-debug-non-zts-20240924",
  "distro": "trixie",
  "arch": "amd64",
  "profile": "optimized",
  "compiler": "gcc-15",
  "cflags": "-O3 -flto -fno-semantic-interposition",
  "cxxflags": "-O3 -flto -fno-semantic-interposition -include cstdint",
  "ldflags": "-flto",
  "built_at": "ISO-8601 timestamp",
  "source": "pecl grpc-1.58.0"
}
```

利用側では以下のように使える状態を目指す。

```dockerfile
FROM ghcr.io/dkkoma/ext-grpc-artifacts:1.58.0-php8.4-trixie-amd64-optimized AS ext-grpc
FROM php:8.4-fpm-trixie

COPY --from=ext-grpc /artifacts/grpc.so /usr/local/lib/php/extensions/no-debug-non-zts-20240924/grpc.so
RUN echo "extension=grpc.so" > /usr/local/etc/php/conf.d/docker-php-ext-grpc.ini
```

PHP extension dirはPHP buildごとに変わるため、利用側Dockerfileで固定パスを直接書くのがつらい場合は、artifact imageから `metadata.json` を読み、build script側で配置先を決める。

## GitHub Actions設計

### trigger

初期:

- `workflow_dispatch`
- `push` to `main`

将来:

- 公式 `grpc` release検知をhookにした定期実行または手動dispatch

### matrix

初期matrix:

```yaml
grpc-version: ["1.58.0", "1.80.0"]
php-version: ["8.4", "8.5"]
arch: ["amd64", "arm64"]
profile: ["pecl", "optimized"]
distro: ["trixie"]
```

### build

- `docker/setup-buildx-action` を使う。
- `docker/build-push-action` でGHCRへpushする。
- `platforms` はmatrixのarchに合わせて単独指定する。
- build cacheはGHCR cacheまたはGitHub Actions cacheを使う。
- `optimized` profileは `gcc-15` をDockerfile内で明示的に使う。

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

`php-grpc-lite` 側のLaravel/FPM bench imageでは、official ext-grpc image targetを以下の方針に変える。

- `pecl install grpc-${version}` をやめる。
- `ghcr.io/dkkoma/ext-grpc-artifacts:<tag>` から `grpc.so` をCOPYする。
- 比較条件に応じて `pecl` / `optimized` artifactを選べるようにする。

これにより、VM上はもちろんGitHub Actions上でもofficial image build時間を大幅に削減できる。

## 初期完了条件

- `1.58.0` / `1.80.0`
- PHP `8.4` / `8.5`
- `amd64` / `arm64`
- `pecl` / `optimized`
- `trixie`

上記すべての組み合わせでGHCR tagがpublishされる。

各tagについて `php -d extension=/artifacts/grpc.so -m` が成功する。

## 注意点

- このrepoは公式 `ext-grpc` の再配布artifactを作るため、license表記とsource情報をREADMEに明記する。
- artifact imageには秘密情報を含めない。
- `grpc 1.58.0` の `-include cstdint` は必要な場合だけ適用する。不要なversionに無条件で入れる必要はない。
- `optimized` profileの性能差は利用側ベンチで評価する。artifact repoではロード可能性とbuild再現性を主目的にする。
- 将来、PHP公式imageのbase distroやPHP ABIが変わった場合はtag体系を壊さず、新しいdistro/profileを追加する。
