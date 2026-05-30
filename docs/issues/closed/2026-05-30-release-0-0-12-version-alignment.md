# 0.0.12 release and runtime version alignment

Status: Closed
Branch: main
Target-Release: 0.0.12

## 目的

`0.0.12` releaseからpackage tagとextension runtime versionを揃え、以後も同じversionとして扱う。

## 背景

これまでrelease tagは `0.0.x`、`Grpc\VERSION` / `phpversion("grpc")` は `0.1.0` として別管理されていた。利用者が実行中のextension versionを確認するとrelease tagと一致せず、install検証やuser-agentの読み取りが紛らわしい。

## スコープ

- `PHP_GRPC_VERSION` を `0.0.12` に更新する。
- `Grpc\VERSION`、`phpversion("grpc")`、php-grpc-lite default user-agentを `PHP_GRPC_VERSION` 由来に揃える。
- README / install guide / PIE install検証Dockerfileの期待値を `0.0.12` に更新する。
- 変更後に開発gateを通し、commit / tag / GitHub releaseを作成する。

## 非スコープ

- 公式 `ext-grpc` のversion番号へ寄せること。
- 過去のbenchmark / diagnostic issueに記録された観測値を書き換えること。
- Packagist側の反映遅延をこの作業内で制御すること。

## 計画

1. Phase 1: version定義とuser-agent定義を `0.0.12` に揃える。
2. Phase 2: install手順と期待値を更新する。
3. Phase 3: 開発gateで検証する。
4. Phase 4: commit、tag、GitHub releaseを作成する。
5. Phase 5: release後にtag / releaseを確認する。

## 進捗

- 2026-05-30 Phase 1 started.
- 2026-05-30 Phase 1 completed: `PHP_GRPC_VERSION` を `0.0.12` に更新し、default user-agentもversion macro由来にした。
- 2026-05-30 Phase 2 started.
- 2026-05-30 Phase 2 completed: README / install guide / PIE install検証Dockerfileの期待値を `0.0.12` に更新した。
- 2026-05-30 Phase 3 started.
- 2026-05-30 Phase 3 completed: development gateとversion assertion付きPHPTを通過した。
- 2026-05-30 Phase 4 started.
- 2026-05-30 Phase 4 blocked: Packagist経由PIE install smokeでdist zipから `src/*` が除外されていることを検出した。
- 2026-05-30 Phase 4 fix started: root layout後のC sourcesをdistへ含めるため `.gitattributes` の `/src export-ignore` を削除する。
- 2026-05-30 Phase 4 completed: `0.0.12` tagをdist修正済みcommitへ差し替え、GitHub Release / Packagist referenceを確認した。
- 2026-05-30 Phase 5 completed: Packagist経由PIE install smokeでbuild/loadとversion一致を確認した。

## 検証

- `./tools/test/check-native-development-gate.sh` passed.
- `./tools/test/check-phpt.sh` passed after adding `phpversion("grpc")` / `Grpc\VERSION` assertions.
- `docker build -f Dockerfile.install-pie --build-arg PHP_GRPC_LITE_PACKAGE=dkkoma/php-grpc-lite:0.0.12 -t php-grpc-lite-install-pie-0.0.12 .` failed before `.gitattributes` fix because Packagist dist did not contain `src/*`.
- `git ls-remote --tags origin 0.0.12` => `6b0d578e735dd7a79ceac940436905faf3bae98b`.
- `gh release view 0.0.12 --repo dkkoma/php-grpc-lite --json tagName,targetCommitish,isDraft,isPrerelease,publishedAt,url,name` => public, not draft, not prerelease.
- Packagist `dkkoma/php-grpc-lite` `0.0.12` source/dist reference => `6b0d578e735dd7a79ceac940436905faf3bae98b`.
- `docker build --no-cache -f Dockerfile.install-pie --build-arg PHP_GRPC_LITE_PACKAGE=dkkoma/php-grpc-lite:0.0.12 -t php-grpc-lite-install-pie-0.0.12 .` passed. PIE built and loaded `grpc`, and `Grpc\VERSION === "0.0.12"` check passed.

## 判断ログ

- `0.0.12` 以後、package tagとextension runtime versionは揃える。
- 公式 `ext-grpc` versionとの一致は目的にしない。
- root layout後、Packagist / GitHub dist zipにはC extension sources under `src/` が必要なため、`.gitattributes` で `/src export-ignore` しない。
- 初回 `0.0.12` tagはdist zip不備を含むcommitを指していたため、ユーザー承認に基づき同じtagをdist修正済みcommitへ差し替えた。

## 修正コミット

- `6e0b676` `0.0.12 release: runtime versionをtagと揃える`
- `6b0d578` `0.0.12 release: distにC sourcesを含める`

## 完了条件

- `PHP_GRPC_VERSION` が `0.0.12`。
- `Grpc\VERSION` と `phpversion("grpc")` が `0.0.12`。
- default user-agentが `php-grpc-lite/0.0.12`。
- `0.0.12` tagとGitHub releaseが存在する。
