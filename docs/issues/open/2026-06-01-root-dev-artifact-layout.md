# Root dev artifact layout

Status: Open

## 目的

repository rootにある開発・検証・診断用artifactを用途別directoryへ移し、production code / extension build surface / 配布metadataが見えやすいroot構造にする。

## 背景

現在のrootには `Dockerfile*`、`bench.php`、`phpunit.xml.dist` など、production codeやpackage配布物ではないファイルが多く並んでいる。`config.m4`、`grpc.c`、`php_grpc.h`、`composer.json` などPHP extension / Composer packageとしてrootにあるべきファイルと混ざっており、初学者にも上級者にも入口が読みにくい。

## スコープ

- root直下のDockerfile群を `docker/` に移す。
- root直下のdiagnostic bench entrypoint `bench.php` を `tools/benchmark/` に移す。
- PHPUnit configを `tests/` 配下に移し、実行コマンドではconfig pathを明示する。
- compose / CI / scripts / README / current guideの参照を更新する。

## 非スコープ

- PHP extension source layoutは変更しない。
- Composer packageの配布surfaceは変更しない。
- 過去issue / research / benchmark記録の歴史的なpath記述は原則として書き換えない。
- Docker imageの中身やbuild semanticsは変更しない。

## 計画

1. ファイル移動と参照更新を行う。
2. YAML / shell / PHP構文を確認する。
3. Docker compose configと代表Docker build pathを確認する。
4. PHPUnit config pathを使った実行導線を確認する。

## 進捗

- 2026-06-01: issue作成。
- 2026-06-01: root直下のDockerfile群を `docker/` へ移動。
- 2026-06-01: `bench.php` を `tools/benchmark/extension-bench.php` へ移動し、autoload pathを更新。
- 2026-06-01: `phpunit.xml.dist` を `tests/phpunit.xml.dist` へ移動し、bootstrap / testsuite pathを更新。
- 2026-06-01: compose / CI / release build script / README / current guideの参照を更新。

## 検証

- `ruby -e 'require "yaml"; Dir[".github/workflows/*.yml"].each { |f| YAML.load_file(f) }; puts "yaml ok"'`: PASS
- `git diff --check`: PASS
- `bash -n tools/release/build-prebuilt-artifact.sh`: PASS
- `git ls-files | awk 'index($0,"/")==0 {print}' | sort`: root直下のgit管理対象から `Dockerfile*`、`bench.php`、`phpunit.xml.dist` が消えていることを確認。
- `docker compose config --quiet`: PASS
- `docker compose config | rg "dockerfile:"`: PASS
  - root contextのDockerfile参照が `docker/Dockerfile*` になっていることを確認。
- `docker compose run --rm dev php -l tools/benchmark/extension-bench.php`: PASS
- `docker compose run --rm dev vendor/bin/phpunit -c tests/phpunit.xml.dist --list-tests`: PASS
- `docker compose run --rm dev php -d extension=/workspace/modules/grpc.so vendor/bin/phpunit -c tests/phpunit.xml.dist`: PASS
  - 30 tests / 109 assertions
- `./tools/release/build-prebuilt-artifact.sh 0.0.13 8.4 nts trixie arm64`: PASS
  - moved path `docker/Dockerfile.release-artifact` を使ってbuildできることを確認。
- `docker compose build dev`: PASS
  - moved path `docker/Dockerfile` を使ってdev imageをbuildできることを確認。
- current docs / scripts reference check: PASS
  - `-f Dockerfile`、`file: Dockerfile`、`dockerfile: Dockerfile` の旧参照がcurrent docs / scriptsには残っていないことを確認。
  - `vendor/bin/phpunit` のcurrent docs / scriptsでは `-c tests/phpunit.xml.dist` を明示していることを確認。

## 判断ログ

- `composer.json`、`config.m4`、`grpc.c`、`php_grpc.h` はpackage / extension build surfaceなのでrootに残す。
- `.gitattributes` は現在 `/Dockerfile` / `/Dockerfile.*` をexport-ignoreしている。Dockerfile群を `docker/` に移した後もComposer distへ入れないため、`/docker` をexport-ignoreへ追加する。
- `phpunit.xml.dist` はroot慣習もあるが、このrepositoryではDocker内の検証コマンドをdocs / AGENTS / CIで明示しているため、`tests/phpunit.xml.dist` に移して `vendor/bin/phpunit -c tests/phpunit.xml.dist` を標準にする。

## 完了条件

- root直下からgit管理対象の `Dockerfile*`、`bench.php`、`phpunit.xml.dist` がなくなる。
- `docker compose config` が通る。
- release artifact build scriptが新しいDockerfile pathを参照している。
- PHPUnit実行導線が新しいconfig pathで動く。
