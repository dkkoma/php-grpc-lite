# verify 0.0.2 release install path

Status: Closed

## 目的

`0.0.2` release後に、GitHub Release、Packagist、CI、PIE install、公式PHP Docker imageでのextension loadを確認する。

## 背景

`0.0.2` をリリース済み。release install pathとしてはローカルsourceではなくPackagist経由の `pie install dkkoma/php-grpc-lite` が主導線になる。

## スコープ

- GitHub Release / tag / Packagist反映確認。
- latest CI run確認。
- Packagist経由PIE install確認。
- source install Dockerfile確認。
- リリース検証用Dockerfileとdocsの整合性修正。

## 非スコープ

- 新しいrelease tag作成。
- benchmark再計測。
- packaging semanticsの大幅変更。

## 進捗

- `git fetch --tags origin` で `0.0.2` tagを取得し、`5756ded0e5401dc570f68d3f9eab8a34810865cd` を指すことを確認した。
- GitHub Release `0.0.2` がdraft/prereleaseではなく公開済みであることを確認した。
- Packagist `dkkoma/php-grpc-lite` に `0.0.2` が反映され、dist zipとsource referenceが `5756ded0e5401dc570f68d3f9eab8a34810865cd` であることを確認した。
- 最新GitHub Actions `Native QA` run `25648014299` がsuccessで、`Development gate` と `C coverage` の両jobがsuccessであることを確認した。
- Packagist経由PIE installを公式 `php:8.4-cli-trixie` imageで検証し、`extension_loaded("grpc")` と `Grpc\VERSION === "0.1.0"` を確認した。
- `Dockerfile.install-grpc` によるsource installも公式PHP imageで検証した。
- `Dockerfile.install-pie` をPackagist package検証用に変更し、`PHP_GRPC_LITE_PACKAGE` build argで特定releaseを指定できるようにした。
- READMEとinstall guideに、`0.0.2` を指定したPIE release検証例を追加した。

## 検証

- `git fetch --tags origin`
- `gh release view 0.0.2 --repo dkkoma/php-grpc-lite --json tagName,targetCommitish,isDraft,isPrerelease,publishedAt,url,name`
- `gh run list --repo dkkoma/php-grpc-lite --limit 8 --json databaseId,headBranch,headSha,event,status,conclusion,workflowName,createdAt,url`
- `gh run view 25648014299 --repo dkkoma/php-grpc-lite --json conclusion,status,url,jobs`
- `curl -fsSL https://repo.packagist.org/p2/dkkoma/php-grpc-lite.json`
- `docker build -f Dockerfile.install-pie --build-arg PHP_GRPC_LITE_PACKAGE=dkkoma/php-grpc-lite:0.0.2 -t php-grpc-lite-install-pie-0.0.2 .`
- `docker run --rm php-grpc-lite-install-pie-0.0.2 php -m | grep -x grpc`
- `docker run --rm php-grpc-lite-install-pie-0.0.2 php -r 'var_dump(extension_loaded("grpc"), defined("Grpc\\VERSION") && constant("Grpc\\VERSION") === "0.1.0");'`
- `docker build -f Dockerfile.install-grpc -t php-grpc-lite-install-grpc-check .`
- `docker run --rm php-grpc-lite-install-grpc-check php -m | grep -x grpc`
- `docker run --rm php-grpc-lite-install-grpc-check php -r 'var_dump(extension_loaded("grpc"), defined("Grpc\\VERSION") && constant("Grpc\\VERSION") === "0.1.0");'`

## 判断ログ

- `Dockerfile.install-pie` はローカルsourceではなくPackagist packageを検証する導線に寄せた。
- PIE 1.4.3 は公式PHP image上で `libtoolize/glibtoolize` missing warningを出すが、phpize/configure/build/installは成功した。現時点では追加依存にせず、実際の成功確認を優先する。
- `0.0.2` package versionとextension runtime `Grpc\VERSION === "0.1.0"` は別物として扱う。

## 完了条件

- `0.0.2` がtag / GitHub Release / Packagistで確認できる。
- CIが成功している。
- Packagist経由PIE installとsource Docker installが成功する。
- README / install guideが検証導線と一致する。

## 修正コミット

このコミット。
