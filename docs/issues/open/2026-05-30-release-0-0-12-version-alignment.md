# 0.0.12 release and runtime version alignment

Status: Open
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

## 検証

- `./tools/test/check-native-development-gate.sh` passed.
- `./tools/test/check-phpt.sh` passed after adding `phpversion("grpc")` / `Grpc\VERSION` assertions.

## 判断ログ

- `0.0.12` 以後、package tagとextension runtime versionは揃える。
- 公式 `ext-grpc` versionとの一致は目的にしない。

## 完了条件

- `PHP_GRPC_VERSION` が `0.0.12`。
- `Grpc\VERSION` と `phpversion("grpc")` が `0.0.12`。
- default user-agentが `php-grpc-lite/0.0.12`。
- `0.0.12` tagとGitHub releaseが存在する。
